#pragma once
#include <mbedtls/base64.h>
#include "api-json.h"
#include "api-tools.h"
#define API_BODY_MAX 65536
static int api_capture_fd=-99;
static unsigned char api_capture[0x90000],api_output[0xD0000];
static size_t api_captured,api_output_size;
static const unsigned char *api_incoming;
static size_t api_incoming_left;
static bool api_backend(int fd,const char *command);
static bool api_append(const void *data,size_t n){if(n>sizeof(api_capture)-api_captured)return false;memcpy(api_capture+api_captured,data,n);api_captured+=n;return true;}
static bool api_read(void *data,size_t n){if(n>api_incoming_left)return false;memcpy(data,api_incoming,n);api_incoming+=n;api_incoming_left-=n;return true;}
static bool api_run(int fd,const char *command){api_captured=0;api_capture_fd=fd;bool ok=api_backend(fd,command);api_capture_fd=-99;return ok;}
static bool api_out(const void *data,size_t n){if(n>sizeof(api_output)-api_output_size)return false;memcpy(api_output+api_output_size,data,n);api_output_size+=n;return true;}
static bool api_literal(const char *s){return api_out(s,strlen(s));}
static bool api_quote(const unsigned char *s,size_t n){if(!api_literal("\""))return false;for(size_t i=0;i<n;i++){unsigned char c=s[i];if(c<32||c=='"'||c=='\\'){char esc[8];snprintf(esc,sizeof(esc),"\\u%04x",c);if(!api_literal(esc))return false;}else if(!api_out(s+i,1))return false;}return api_literal("\"");}
static bool api_property(const ApiJson *j,int args,const char *key,char *out,size_t n){return aj_text(j,aj_get(j,args,key),out,n);}
static bool api_num(const ApiJson *j,int args,const char *key,double *value,double low,double high,double fallback){int at=aj_get(j,args,key);if(at<0){*value=fallback;return true;}return aj_number(j,at,value)&&*value>=low&&*value<=high;}
static bool api_args(const ApiJson *j,int args,const char *allowed){if(args<0)return true;if(j->nodes[args].kind!='{')return false;for(int at=j->nodes[args].child;at>=0&&at<j->nodes[args].next;at=j->nodes[at+1].next){char key[128],needle[132];if(!aj_text(j,at,key,sizeof(key)))return false;snprintf(needle,sizeof(needle),"|%s|",key);if(!strstr(allowed,needle))return false;}return true;}
static bool api_path(const ApiJson *j,int args,const char *key,char *out,size_t n){char decoded[768];if(!api_property(j,args,key,decoded,sizeof(decoded))||decoded[0]!='/'||strlen(decoded)*2+1>n)return false;sd_hex(out,(const unsigned char*)decoded,strlen(decoded));return true;}
static bool api_upload(int fd,const char *path,const char *expected,const unsigned char *body,size_t size){
    char sha[65],cmd[2048],token[40];unsigned char digest[32];Sha256Context ctx;sha256ContextCreate(&ctx);sha256ContextUpdate(&ctx,body,size);sha256ContextGetHash(&ctx,digest);sd_hex(sha,digest,32);
    snprintf(cmd,sizeof(cmd),"fsBegin %s %lu %s %s",path,(unsigned long)size,sha,expected);if(!api_run(fd,cmd))return false;
    ApiJson json;if(!aj_parse(&json,(const char*)api_capture,api_captured)||!api_property(&json,0,"upload",token,sizeof(token)))return true;
    if(size){api_incoming=body;api_incoming_left=size;snprintf(cmd,sizeof(cmd),"fsChunk %s 0 %lu",token,(unsigned long)size);if(!api_run(fd,cmd))return false;}
    snprintf(cmd,sizeof(cmd),"fsCommit %s",token);return api_run(fd,cmd);
}
#include "api-input.h"
#include "api-config.h"
static bool api_tool(int fd,const char *name,const ApiJson *j,int args){
    char cmd[2048],a[1536],b[1536],hash[80];double x,y,ms;
    if(!strcmp(name,"get_datetime")||!strcmp(name,"get_auto_time")||!strcmp(name,"set_datetime")||!strcmp(name,"set_auto_time")||!strcmp(name,"set_dns")||!strcmp(name,"dns_update_status"))return api_config_tool(fd,name,j,args);
    if(!strcmp(name,"network_status"))return api_args(j,args,"")&&api_run(fd,"networkInfo");
    if(!strcmp(name,"lifecycle_status"))return api_args(j,args,"")&&api_run(fd,"lifecycleInfo");
    if(!strcmp(name,"list_installed_titles")){double cursor;if(!api_args(j,args,"|cursor|")||!api_num(j,args,"cursor",&cursor,0,100000,0)||floor(cursor)!=cursor)return false;snprintf(cmd,sizeof(cmd),"listTitles %u",(unsigned)cursor);return api_run(fd,cmd);}
    if(!strcmp(name,"input_status")||!strcmp(name,"cancel_input")||!strcmp(name,"renew_input")||!strcmp(name,"run_input")||!strcmp(name,"hold_input")||!strcmp(name,"tap_sequence"))return api_input_job(fd,name,j,args);
    if(!strcmp(name,"file_job_status")||!strcmp(name,"cancel_file_job")){if(!api_args(j,args,"|job|")||!api_property(j,args,"job",a,40)||strlen(a)!=32)return false;for(unsigned i=0;i<32;i++)if(sd_nibble(a[i])<0)return false;snprintf(cmd,sizeof(cmd),"%s %s",!strcmp(name,"file_job_status")?"fsTaskInfo":"fsTaskCancel",a);return api_run(fd,cmd);}
    if(!strcmp(name,"status"))return api_args(j,args,"")&&api_run(fd,"systemStatus");
    if(!strcmp(name,"screenshot"))return api_args(j,args,"")&&api_run(fd,"pixelPeek");
    if(!strcmp(name,"clear_input"))return api_args(j,args,"")&&api_run(fd,"inputClear");
    if(!strcmp(name,"set_stick")){if(!api_args(j,args,"|side||x||y||duration_ms|")||!api_property(j,args,"side",a,sizeof(a))||(!strcmp(a,"left")?false:strcmp(a,"right")!=0)||aj_get(j,args,"x")<0||aj_get(j,args,"y")<0||!api_num(j,args,"x",&x,-1,1,0)||!api_num(j,args,"y",&y,-1,1,0)||!api_num(j,args,"duration_ms",&ms,1,10000,100)||floor(ms)!=ms)return false;if(ms>1000){snprintf(cmd,sizeof(cmd),"inputStart 0:%d,%d,%d,%d:%u:0",!strcmp(a,"left")?(int)round(x*32767):0,!strcmp(a,"left")?(int)round(y*32767):0,!strcmp(a,"right")?(int)round(x*32767):0,!strcmp(a,"right")?(int)round(y*32767):0,(unsigned)ms);return api_run(fd,cmd);}snprintf(cmd,sizeof(cmd),"stick %c %d %d %u",!strcmp(a,"left")?'L':'R',(int)round(x*32767),(int)round(y*32767),(unsigned)ms);return api_run(fd,cmd);}
    if(!strcmp(name,"tap_buttons")){if(!api_args(j,args,"|buttons||duration_ms|")||!api_num(j,args,"duration_ms",&ms,1,10000,100)||floor(ms)!=ms)return false;int list=aj_get(j,args,"buttons");if(list<0||j->nodes[list].kind!='[')return false;char names[8][32];unsigned count=0;for(int at=j->nodes[list].child;at>=0&&at<j->nodes[list].next;at=j->nodes[at].next){u64 mask;if(count==8||!aj_text(j,at,names[count],32)||!button(names[count],&mask))return false;for(unsigned k=0;k<count;k++)if(!strcmp(names[k],names[count]))return false;count++;}if(!count)return false;if(ms>1000){u64 mask=0,bit;for(unsigned i=0;i<count;i++){button(names[i],&bit);mask|=bit;}snprintf(cmd,sizeof(cmd),"inputStart %llx:0,0,0,0:%u:0",(unsigned long long)mask,(unsigned)ms);return api_run(fd,cmd);}if(count==1&&!strcmp(names[0],"HOME")&&ms==100)return api_run(fd,"clickSeq W500,+HOME,W300,-HOME,W500");strcpy(cmd,"clickSeq ");for(unsigned i=0;i<count;i++){strcat(cmd,"+");strcat(cmd,names[i]);strcat(cmd,",");}snprintf(a,sizeof(a),"W%u,",(unsigned)ms);strcat(cmd,a);for(unsigned i=0;i<count;i++){strcat(cmd,"-");strcat(cmd,names[i]);strcat(cmd,",");}strcat(cmd,"W100");return api_run(fd,cmd);}
    if(!strcmp(name,"set_system")){if(!api_args(j,args,"|name||value|")||!api_property(j,args,"name",a,32))return false;int value=aj_get(j,args,"value");if(!strcmp(a,"brightness")||!strcmp(a,"volume")){if(!aj_number(j,value,&x)||x<0||x>1)return false;snprintf(b,sizeof(b),"%.6f",x);}else if(!strcmp(a,"auto_brightness")){if(value<0||(j->nodes[value].kind!='t'&&j->nodes[value].kind!='f'))return false;strcpy(b,j->nodes[value].kind=='t'?"true":"false");}else if(!strcmp(a,"theme")){if(!aj_text(j,value,b,sizeof(b))||(strcmp(b,"light")&&strcmp(b,"dark")))return false;}else if(!strcmp(a,"nickname")){char text[128];if(!aj_text(j,value,text,sizeof(text))||!text[0])return false;for(const unsigned char *p=(void*)text;*p;p++)if(*p<32||*p==127)return false;sd_hex(b,(void*)text,strlen(text));}else return false;snprintf(cmd,sizeof(cmd),"systemSet %.31s %.1535s",a,b);return api_run(fd,cmd);}
    if(!strcmp(name,"move_file")){if(!api_args(j,args,"|source||destination||expected_sha256|")||!api_path(j,args,"source",a,sizeof(a))||!api_path(j,args,"destination",b,sizeof(b))||!api_property(j,args,"expected_sha256",hash,sizeof(hash))||!sd_digest(hash)||strlen(a)+strlen(b)+80>=sizeof(cmd))return false;strcpy(cmd,"fsMove ");strcat(cmd,a);strcat(cmd," ");strcat(cmd,b);strcat(cmd," ");strcat(cmd,hash);return api_run(fd,cmd);}
    if(!api_path(j,args,"path",a,sizeof(a)))return false;
    if(!strcmp(name,"tail_file")){if(!api_args(j,args,"|path||size|")||!api_num(j,args,"size",&y,1,65536,4096)||floor(y)!=y)return false;snprintf(cmd,sizeof(cmd),"fsTail %s %u",a,(unsigned)y);return api_run(fd,cmd);}
    if(!strcmp(name,"start_hash")){if(!api_args(j,args,"|path|"))return false;snprintf(cmd,sizeof(cmd),"fsHashStart %s",a);return api_run(fd,cmd);}
    if(!strcmp(name,"read_file")){if(!api_args(j,args,"|path||offset||size|")||!api_num(j,args,"offset",&x,0,9007199254740991.0,0)||!api_num(j,args,"size",&y,1,65536,65536)||floor(x)!=x||floor(y)!=y)return false;snprintf(cmd,sizeof(cmd),"fsRead %s %llu %u",a,(unsigned long long)x,(unsigned)y);return api_run(fd,cmd);}
    if(!strcmp(name,"write_file")){if(!api_args(j,args,"|path||content_base64||expected_sha256|"))return false;int content=aj_get(j,args,"content_base64");if(content<0||j->nodes[content].kind!='s')return false;size_t encoded=(size_t)(j->nodes[content].end-j->nodes[content].start-2),size;if(encoded>65536)return false;static unsigned char decoded[49152];if(mbedtls_base64_decode(decoded,sizeof(decoded),&size,(const unsigned char*)j->text+j->nodes[content].start+1,encoded))return false;strcpy(hash,"missing");if(aj_get(j,args,"expected_sha256")>=0&&!api_property(j,args,"expected_sha256",hash,sizeof(hash)))return false;if(strcmp(hash,"missing")&&!sd_digest(hash))return false;return api_upload(fd,a,hash,decoded,size);}
    if(!strcmp(name,"delete_file")){if(!api_args(j,args,"|path||expected_sha256|")||!api_property(j,args,"expected_sha256",hash,sizeof(hash))||!sd_digest(hash))return false;snprintf(cmd,sizeof(cmd),"fsDelete %s %s",a,hash);return api_run(fd,cmd);}
    if(!strcmp(name,"list_directory")){double cursor;if(!api_args(j,args,"|path||cursor|")||!api_num(j,args,"cursor",&cursor,0,9007199254740991.0,0)||floor(cursor)!=cursor)return false;snprintf(cmd,sizeof(cmd),"fsList %s %llu",a,(unsigned long long)cursor);return api_run(fd,cmd);}
    if(!api_args(j,args,"|path|"))return false;
    const char *op=!strcmp(name,"list_directory")?"fsList":!strcmp(name,"stat_file")?"fsStat":!strcmp(name,"hash_file")?"fsHash":!strcmp(name,"make_directory")?"fsMkdir":NULL;
    if(!op)return false;
    snprintf(cmd,sizeof(cmd),"%s %s%s",op,a,!strcmp(name,"list_directory")?" 0":"");return api_run(fd,cmd);
}
static void api_error(const char *id,int code,const char *message){api_output_size=0;char prefix[256];snprintf(prefix,sizeof(prefix),"{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":",id,code);api_literal(prefix);api_quote((const unsigned char*)message,strlen(message));api_literal("}}");}
static int api_mcp(int fd,const unsigned char *body,size_t size){
    ApiJson j;api_output_size=0;if(!aj_parse(&j,(const char*)body,size)||j.nodes[0].kind!='{'){api_error("null",-32700,"Invalid JSON");return 400;}
    char id[256]="null",method[80],version[32];int identifier=aj_get(&j,0,"id");if(identifier>=0){ApiNode n=j.nodes[identifier];int length=n.end-n.start;if((n.kind!='s'&&n.kind!='n')||length>250){api_error("null",-32600,"Invalid ID");return 400;}memcpy(id,j.text+n.start,(size_t)length);id[length]=0;}
    if(!api_property(&j,0,"jsonrpc",version,sizeof(version))||strcmp(version,"2.0")||!api_property(&j,0,"method",method,sizeof(method))){api_error(id,-32600,"Invalid request");return 400;}
    if(identifier<0)return 202;
    char prefix[320];snprintf(prefix,sizeof(prefix),"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":",id);api_literal(prefix);
    if(!strcmp(method,"initialize")){int params=aj_get(&j,0,"params");char protocol[32]="2025-11-25";if(api_property(&j,params,"protocolVersion",version,sizeof(version))&&(!strcmp(version,"2025-11-25")||!strcmp(version,"2025-06-18")||!strcmp(version,"2025-03-26")))strcpy(protocol,version);char result[256];snprintf(result,sizeof(result),"{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"switch-control\",\"version\":\"0.5.0\"}}",protocol);api_literal(result);}
    else if(!strcmp(method,"ping"))api_literal("{}");
    else if(!strcmp(method,"tools/list"))api_literal(api_tools);
    else if(!strcmp(method,"tools/call")){int params=aj_get(&j,0,"params"),args=aj_get(&j,params,"arguments");char name[80];if(!api_property(&j,params,"name",name,sizeof(name))||!api_tool(fd,name,&j,args)){api_error(id,-32602,"Invalid tool or arguments");return 200;}bool error=api_captured>=3&&!memcmp(api_capture,"ERR",3);if(api_captured>8&&api_capture[0]=='{'){ApiJson response;if(aj_parse(&response,(char*)api_capture,api_captured)&&aj_get(&response,0,"error")>=0)error=true;}
        api_literal("{\"content\":[{");
        if(!strcmp(name,"screenshot")&&!error){unsigned char *newline=memchr(api_capture,'\n',api_captured);if(!newline){api_error(id,-32603,"Invalid screenshot");return 200;}size_t n=api_captured-(size_t)(newline+1-api_capture);api_literal("\"type\":\"image\",\"mimeType\":\"image/jpeg\",\"data\":\"");size_t encoded;if(mbedtls_base64_encode(api_output+api_output_size,sizeof(api_output)-api_output_size,&encoded,newline+1,n)){api_error(id,-32603,"Image too large");return 200;}api_output_size+=encoded;api_literal("\"");}
        else if((!strcmp(name,"read_file")||!strcmp(name,"tail_file"))&&!error){unsigned char *newline=memchr(api_capture,'\n',api_captured);if(!newline){api_error(id,-32603,"Invalid file");return 200;}size_t n=api_captured-(size_t)(newline+1-api_capture);static unsigned char encoded[90000];size_t count;if(mbedtls_base64_encode(encoded,sizeof(encoded)-1,&count,newline+1,n))return 500;encoded[count]=0;api_literal("\"type\":\"text\",\"text\":");api_quote(encoded,count);}
        else {api_literal("\"type\":\"text\",\"text\":");api_quote(api_capture,api_captured);}
        api_literal(error?"}],\"isError\":true}":"}],\"isError\":false}");}
    else {api_error(id,-32601,"Method not found");return 200;}
    api_literal("}");return 200;
}
