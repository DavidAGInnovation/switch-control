#pragma once
#include "api-core.h"
typedef struct {unsigned char data[8192+API_BODY_MAX+1];size_t used,header,body;int error;bool close;char method[12],target[2048];} ApiRequest;
static bool api_host_allowed(const char *host){char expected[100];snprintf(expected,sizeof(expected),"%s:7443",disc_host);if(!strcasecmp(host,expected))return true;struct in_addr address={.s_addr=disc_ip};snprintf(expected,sizeof(expected),"%s:7443",inet_ntoa(address));return !strcasecmp(host,expected);}
static int api_headers(ApiRequest *r,const unsigned char key[32]) {
    size_t end=0;for(size_t i=0;i+3<r->used&&i<8192;i++)if(!memcmp(r->data+i,"\r\n\r\n",4)){end=i+4;break;}
    if(!end)return r->used>=8192?431:0;
    if(memchr(r->data,0,end))return 400;
    char copy[8193];memcpy(copy,r->data,end);copy[end]=0;char *save=NULL,*line=strtok_r(copy,"\r\n",&save),version[16],extra;
    if(!line||sscanf(line,"%11s %2047s %15s %c",r->method,r->target,version,&extra)!=3||strcmp(version,"HTTP/1.1")||r->target[0]!='/')return 400;
    bool host=false,auth=false,have_auth=false,have_length=false;char token[65];sd_hex(token,key,32);size_t length=0;bool content_json=false,accept_json=false,accept_sse=false;
    while((line=strtok_r(NULL,"\r\n",&save))){char *colon=strchr(line,':');if(!colon||line[0]==' '||line[0]=='\t')return 400;*colon++=0;while(*colon==' '||*colon=='\t')colon++;size_t z=strlen(colon);while(z&&(colon[z-1]==' '||colon[z-1]=='\t'))colon[--z]=0;
        if(!strcasecmp(line,"Host")){if(host||!api_host_allowed(colon))return 403;host=true;}
        else if(!strcasecmp(line,"Origin")){if(strncasecmp(colon,"https://",8)||!api_host_allowed(colon+8))return 403;}
        else if(!strcasecmp(line,"Connection")){if(!strcasecmp(colon,"close"))r->close=true;}
        else if(!strcasecmp(line,"Authorization")){if(have_auth)return 400;have_auth=true;unsigned diff=0;if(strlen(colon)==71&&!memcmp(colon,"Bearer ",7)){for(unsigned i=0;i<64;i++)diff|=(unsigned char)colon[7+i]^(unsigned char)token[i];auth=!diff;}}
        else if(!strcasecmp(line,"Content-Length")){uint64_t n;if(have_length||!sd_number(colon,&n)||n>API_BODY_MAX)return 413;length=(size_t)n;have_length=true;}
        else if(!strcasecmp(line,"Transfer-Encoding"))return 400;
        else if(!strcasecmp(line,"Content-Type"))content_json=!strncasecmp(colon,"application/json",16)&&(colon[16]==0||colon[16]==';');
        else if(!strcasecmp(line,"Accept")){accept_json=strstr(colon,"application/json")!=NULL;accept_sse=strstr(colon,"text/event-stream")!=NULL;}
        else if(!strcasecmp(line,"MCP-Protocol-Version")&&strcmp(colon,"2025-11-25")&&strcmp(colon,"2025-06-18")&&strcmp(colon,"2025-03-26"))return 400;
    }
    if(!host)return 400;
    if(!strcmp(r->method,"GET")&&!strcmp(r->target,"/"))return 404;
    bool public=!strcmp(r->method,"GET")&&!strcmp(r->target,"/certificate");
    if(!public&&!auth)return 401;
    if(!strcmp(r->target,"/mcp")&&!strcmp(r->method,"POST")&&(!content_json||!accept_json||!accept_sse))return 406;
    r->header=end;r->body=length;if(r->used>end+length)return 400;return 200;
}
static bool api_query(const char *target,const char *key,char *out,size_t capacity){const char *q=strchr(target,'?');if(!q)return false;q++;size_t k=strlen(key);while(*q){const char *end=strchr(q,'&');if(!end)end=q+strlen(q);if((size_t)(end-q)>k&&q[k]=='='&&!memcmp(q,key,k)){size_t used=0;for(const char *p=q+k+1;p<end;p++){unsigned char c=(unsigned char)*p;if(c=='%'){if(end-p<3)return false;int a=sd_nibble(p[1]),b=sd_nibble(p[2]);if(a<0||b<0)return false;c=(unsigned char)((a<<4)|b);p+=2;}else if(c=='+')c=' ';if(!c||c<32||used+1>=capacity)return false;out[used++]=(char)c;}out[used]=0;return true;}q=*end?end+1:end;}return false;}
static bool api_capture_json(void){if(api_captured==3&&!memcmp(api_capture,"OK\n",3))return api_literal("{\"ok\":true}");if(api_captured>=3&&!memcmp(api_capture,"ERR",3))return api_literal("{\"error\":")&&api_quote(api_capture,api_captured)&&api_literal("}");return api_out(api_capture,api_captured);}
static int api_rest(int fd,ApiRequest *r){
    api_output_size=0;const unsigned char *body=r->data+r->header;size_t size=r->body;char route[2048];strcpy(route,r->target);char *q=strchr(route,'?');if(q)*q=0;bool get=!strcmp(r->method,"GET"),post=!strcmp(r->method,"POST"),put=!strcmp(r->method,"PUT"),del=!strcmp(r->method,"DELETE");char command[2048],path[768],hex[1536],expected[80];ApiJson json;
    if(!strcmp(route,"/mcp")){if(!post)return 405;return api_mcp(fd,body,size);}
    if(!strcmp(route,"/tools")&&get){api_literal(api_tools);return 200;}
    if((!strcmp(route,"/status")||!strcmp(route,"/settings"))&&get){if(!api_run(fd,"systemStatus"))return 500;
        api_capture_json();return 200;}
    if((!strcmp(route,"/network")||!strcmp(route,"/network/dns"))&&get){if(!api_run(fd,"networkInfo"))return 500;api_capture_json();return 200;}
    if(!strcmp(route,"/network/dns/update")&&get){if(!api_run(fd,"dnsStatus"))return 500;api_capture_json();return 200;}
    if((!strcmp(route,"/settings/datetime")||!strcmp(route,"/settings/auto-time"))&&get){if(!api_run(fd,"clockInfo"))return 500;api_capture_json();return 200;}
    if(!strcmp(route,"/lifecycle")&&get){if(!api_run(fd,"lifecycleInfo"))return 500;api_capture_json();return 200;}
    if(!strcmp(route,"/titles")&&get){char cursor[32]="0";api_query(r->target,"cursor",cursor,sizeof(cursor));uint64_t n;if(!sd_number(cursor,&n)||n>100000)return 400;snprintf(command,sizeof(command),"listTitles %u",(unsigned)n);if(!api_run(fd,command))return 500;api_capture_json();return 200;}
    if(!strcmp(route,"/screenshot")&&get){if(!api_run(fd,"pixelPeek"))return 500;unsigned char *line=memchr(api_capture,'\n',api_captured);if(!line||memcmp(api_capture,"JPEG ",5))return 500;size_t n=api_captured-(size_t)(line+1-api_capture);api_out(line+1,n);return 200;}
    const char *tool=NULL;if(!strncmp(route,"/tools/",7)&&post)tool=route+7;
    else if(!strcmp(route,"/input/tap")&&post)tool="tap_buttons";
    else if(!strcmp(route,"/input/sequence")&&post)tool="tap_sequence";
    else if(!strcmp(route,"/input/stick")&&post)tool="set_stick";
    else if(!strcmp(route,"/input/run")&&post)tool="run_input";
    else if(!strcmp(route,"/input/hold")&&post)tool="hold_input";
    else if(!strcmp(route,"/input/renew")&&post)tool="renew_input";
    else if(!strcmp(route,"/input/cancel")&&post)tool="cancel_input";
    else if(!strcmp(route,"/input/status")&&post)tool="input_status";
    else if(!strcmp(route,"/input/clear")&&post)tool="clear_input";
    else if(!strcmp(route,"/network/dns")&&post)tool="set_dns";
    else if(!strcmp(route,"/settings/datetime")&&post)tool="set_datetime";
    else if(!strcmp(route,"/settings/auto-time")&&post)tool="set_auto_time";
    else if(!strcmp(route,"/settings")&&post)tool="set_system";
    if(tool){const char *text=size?(const char*)body:"{}";if(!aj_parse(&json,text,size?size:2)||json.nodes[0].kind!='{'||!api_tool(fd,tool,&json,0))return 400;api_capture_json();return 200;}
    if(!strcmp(route,"/uploads")&&post){if(!aj_parse(&json,(const char*)body,size)||!api_path(&json,0,"path",hex,sizeof(hex))||!api_property(&json,0,"sha256",expected,sizeof(expected))||!sd_digest(expected))return 400;double count;if(aj_get(&json,0,"size")<0||!api_num(&json,0,"size",&count,0,(double)SD_MAX_FILE,0)||floor(count)!=count)return 400;char old[80]="missing";if(aj_get(&json,0,"expected_sha256")>=0&&!api_property(&json,0,"expected_sha256",old,sizeof(old)))return 400;if(strcmp(old,"missing")&&!sd_digest(old))return 400;snprintf(command,sizeof(command),"fsBegin %s %lu %s %s",hex,(unsigned long)count,expected,old);if(!api_run(fd,command))return 500;
        api_capture_json();return 200;}
    if(!strncmp(route,"/file-jobs/",11)){const char *id=route+11;if(strlen(id)!=32)return 400;for(unsigned i=0;i<32;i++)if(sd_nibble(id[i])<0)return 400;if(!get&&!del)return 405;snprintf(command,sizeof(command),"%s %s",del?"fsTaskCancel":"fsTaskInfo",id);if(!api_run(fd,command))return 500;api_capture_json();return 200;}
    if(!strncmp(route,"/uploads/",9)){const char *id=route+9;size_t z=strcspn(id,"/");if(z!=32)return 400;char token[33];memcpy(token,id,32);token[32]=0;for(unsigned i=0;i<32;i++)if(sd_nibble(token[i])<0)return 400;
        if(get&&!id[32])snprintf(command,sizeof(command),"fsUploadInfo %s",token);
        else if(post&&!strcmp(id+32,"/commit"))snprintf(command,sizeof(command),"fsCommit %s",token);
        else if(del&&!id[32])snprintf(command,sizeof(command),"fsAbort %s",token);
        else if(put&&!id[32]){char offset[32];uint64_t n;if(!api_query(r->target,"offset",offset,sizeof(offset))||!sd_number(offset,&n)||!size)return 400;snprintf(command,sizeof(command),"fsChunk %s %llu %lu",token,(unsigned long long)n,(unsigned long)size);api_incoming=body;api_incoming_left=size;}
        else return 405;
        if(!api_run(fd,command))return 500;
        api_capture_json();return 200;
    }
    if(!strcmp(route,"/files")||!strcmp(route,"/directory")||!strcmp(route,"/hash")||!strcmp(route,"/stat")){
        if(!api_query(r->target,"path",path,sizeof(path)))return 400;
        sd_hex(hex,(void*)path,strlen(path));
        if(!strcmp(route,"/directory")&&get){char cursor[32]="0";uint64_t n;if(strstr(r->target,"cursor=")&&!api_query(r->target,"cursor",cursor,sizeof(cursor)))return 400;if(!sd_number(cursor,&n))return 400;snprintf(command,sizeof(command),"fsList %s %s",hex,cursor);}
        else if(!strcmp(route,"/directory")&&post)snprintf(command,sizeof(command),"fsMkdir %s",hex);
        else if(!strcmp(route,"/hash")&&get)snprintf(command,sizeof(command),"fsHash %s",hex);
        else if(!strcmp(route,"/stat")&&get)snprintf(command,sizeof(command),"fsStat %s",hex);
        else if(!strcmp(route,"/files")&&get&&strstr(r->target,"tail=")){char count[32];uint64_t n;if(!api_query(r->target,"tail",count,sizeof(count))||!sd_number(count,&n)||!n||n>65536)return 400;snprintf(command,sizeof(command),"fsTail %s %s",hex,count);}
        else if(!strcmp(route,"/files")&&get){char offset[32]="0",count[32]="65536";uint64_t at,n;api_query(r->target,"offset",offset,sizeof(offset));api_query(r->target,"size",count,sizeof(count));if(!sd_number(offset,&at)||!sd_number(count,&n)||!n||n>65536)return 400;snprintf(command,sizeof(command),"fsRead %s %s %s",hex,offset,count);}
        else if(!strcmp(route,"/files")&&(put||del)){strcpy(expected,"missing");api_query(r->target,"expected_sha256",expected,sizeof(expected));if(strcmp(expected,"missing")&&!sd_digest(expected))return 400;if(del){if(!sd_digest(expected))return 400;snprintf(command,sizeof(command),"fsDelete %s %s",hex,expected);}else {if(!api_upload(fd,hex,expected,body,size))return 500;
        api_capture_json();return 200;}}
        else return 405;
        if(!api_run(fd,command))return 500;
        if(get&&!strcmp(route,"/files")){unsigned char *line=memchr(api_capture,'\n',api_captured);if(!line||memcmp(api_capture,"FILE ",5)){api_out(api_capture,api_captured);return 409;}api_out(line+1,api_captured-(size_t)(line+1-api_capture));}else api_capture_json();return 200;
    }
    return 404;
}
