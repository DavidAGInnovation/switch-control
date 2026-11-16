#pragma once
/* Bounded DNS-SD responder. No keys or private console identifiers are
 * advertised. A random persistent ID keeps multiple consoles distinct. */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <strings.h>
#define DISC_SERVICE "_switch-control._tcp.local"
static int disc_socket=-1;
static u32 disc_ip;
static bool disc_attempted;
static u64 disc_tick;
static char disc_id[13],disc_host[64],disc_instance[96];
static unsigned disc_u16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
static void disc_put16(unsigned char *p,unsigned n){p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n;}
static bool disc_name(const unsigned char *data,size_t size,size_t *offset,char *out,size_t capacity) {
    size_t at=*offset,used=0,after=0;unsigned jumps=0,labels=0;
    for(;;){if(at>=size||++labels>128)return false;unsigned n=data[at++];if((n&0xc0)==0xc0){if(at>=size||++jumps>8)return false;if(!after)after=at+1;at=((n&63)<<8)|data[at];continue;}if(n&0xc0||n>63||at+n>size)return false;if(!n){*offset=after?after:at;if(used&&out[used-1]=='.')used--;out[used]=0;return true;}if(used+n+1>=capacity)return false;memcpy(out+used,data+at,n);used+=n;out[used++]='.';at+=n;}
}
static bool disc_encode(unsigned char *out,size_t *at,size_t cap,const char *name) {
    const char *p=name;while(*p){const char *end=strchr(p,'.');size_t n=end?(size_t)(end-p):strlen(p);if(!n||n>63||*at+n+2>cap)return false;out[(*at)++]=(unsigned char)n;memcpy(out+*at,p,n);*at+=n;if(!end)break;p=end+1;}if(*at>=cap)return false;out[(*at)++]=0;return true;
}
static bool disc_record(unsigned char *out,size_t *at,const char *name,unsigned type,bool legacy,const unsigned char *data,size_t size) {
    if(!disc_encode(out,at,1400,name)||*at+10+size>1400)return false;
    disc_put16(out+*at,type);disc_put16(out+*at+2,type==12||legacy?1:0x8001);out[*at+4]=0;out[*at+5]=0;out[*at+6]=0;out[*at+7]=(unsigned char)(legacy?10:120);disc_put16(out+*at+8,(unsigned)size);*at+=10;memcpy(out+*at,data,size);*at+=size;return true;
}
static size_t disc_response(const unsigned char *query,size_t length,unsigned char out[1400],bool legacy) {
    if(length<12||(query[2]&0x80)||disc_u16(query+4)>16)return 0;
    size_t at=12;bool wanted=false;char name[256];unsigned count=disc_u16(query+4);
    for(unsigned i=0;i<count;i++){if(!disc_name(query,length,&at,name,sizeof(name))||at+4>length)return 0;unsigned type=disc_u16(query+at),cls=disc_u16(query+at+2)&0x7fff;at+=4;if(cls==1&&(type==1||type==12||type==16||type==33||type==255)&&(!strcasecmp(name,DISC_SERVICE)||!strcasecmp(name,disc_host)||!strcasecmp(name,disc_instance)||!strcasecmp(name,"_services._dns-sd._udp.local")))wanted=true;}
    if(!wanted||at>900)return 0;
    memset(out,0,12);out[2]=0x84;size_t used=12;if(legacy){out[0]=query[0];out[1]=query[1];disc_put16(out+4,count);memcpy(out+12,query+12,at-12);used=at;}
    unsigned char data[300];size_t n=0;if(!disc_encode(data,&n,sizeof(data),disc_instance)||!disc_record(out,&used,DISC_SERVICE,12,legacy,data,n))return 0;
    memset(data,0,6);disc_put16(data+4,6012);n=6;if(!disc_encode(data,&n,sizeof(data),disc_host)||!disc_record(out,&used,disc_instance,33,legacy,data,n))return 0;
    const char *txt[]={"version=0.5.1","auth=tls-psk","path=control","protocol=zaw1","https=7443","mcp=/mcp"};n=0;for(unsigned i=0;i<6;i++){size_t k=strlen(txt[i]);data[n++]=(unsigned char)k;memcpy(data+n,txt[i],k);n+=k;}
    if(!disc_record(out,&used,disc_instance,16,legacy,data,n)||!disc_record(out,&used,disc_host,1,legacy,(const unsigned char*)&disc_ip,4))return 0;
    disc_put16(out+6,4);return used;
}
static bool disc_identity(void) {
    if(disc_id[0])return true;
    const char *path="sdmc:/config/za-usb-control/device.id";FILE *f=fopen(path,"rb");
    if(f){size_t n=fread(disc_id,1,12,f);int extra=fgetc(f);fclose(f);if(n!=12||extra!=EOF)return false;for(unsigned i=0;i<12;i++)if(!((disc_id[i]>='0'&&disc_id[i]<='9')||(disc_id[i]>='a'&&disc_id[i]<='f'))){disc_id[0]=0;return false;}}
    else {unsigned char random[6];randomGet(random,sizeof(random));const char *hex="0123456789abcdef";for(unsigned i=0;i<6;i++){disc_id[i*2]=hex[random[i]>>4];disc_id[i*2+1]=hex[random[i]&15];}int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){disc_id[0]=0;return false;}bool ok=write(fd,disc_id,12)==12&&!fsync(fd);if(close(fd))ok=false;if(!ok){disc_id[0]=0;return false;}}
    snprintf(disc_host,sizeof(disc_host),"switch-%s.local",disc_id);snprintf(disc_instance,sizeof(disc_instance),"Switch Control %s.%s",disc_id,DISC_SERVICE);return true;
}
static void discovery_poll(void) {
    u64 now=svcGetSystemTick();if(!disc_tick||armTicksToNs(now-disc_tick)>=1000000000ULL){disc_tick=now;u32 ip=0;Result rc=nifmGetCurrentIpAddress(&ip);if(R_FAILED(rc))ip=0;
        if(ip!=disc_ip){if(disc_socket>=0)close(disc_socket);disc_socket=-1;disc_ip=ip;disc_attempted=false;}
        if(ip&&!disc_attempted){disc_attempted=true;if(disc_identity()){int fd=socket(AF_INET,SOCK_DGRAM,0);int yes=1;struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(5353),.sin_addr.s_addr=htonl(INADDR_ANY)};struct ip_mreq group={.imr_multiaddr.s_addr=inet_addr("224.0.0.251"),.imr_interface.s_addr=htonl(INADDR_ANY)};
            if(fd>=0&&!setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes))&&!bind(fd,(void*)&a,sizeof(a))&&!setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&group,sizeof(group))&&!fcntl(fd,F_SETFL,O_NONBLOCK)){unsigned char ttl=255;setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl));disc_socket=fd;}else if(fd>=0)close(fd);}}
    }
    if(disc_socket<0)return;
    for(unsigned i=0;i<2;i++){unsigned char query[1500],response[1400];struct sockaddr_in peer;socklen_t size=sizeof(peer);ssize_t n=recvfrom(disc_socket,query,sizeof(query),0,(void*)&peer,&size);if(n<=0)break;bool legacy=ntohs(peer.sin_port)!=5353;size_t result=disc_response(query,(size_t)n,response,legacy);if(!result)continue;
        /* QU questions and legacy queries request a unicast reply. */
        bool unicast=legacy;size_t offset=12;char name[256];for(unsigned q=0;q<disc_u16(query+4);q++){if(!disc_name(query,(size_t)n,&offset,name,sizeof(name))||offset+4>(size_t)n)break;if(query[offset+2]&0x80)unicast=true;offset+=4;}
        if(!unicast){peer.sin_addr.s_addr=inet_addr("224.0.0.251");peer.sin_port=htons(5353);}sendto(disc_socket,response,result,0,(void*)&peer,sizeof(peer));
    }
}
