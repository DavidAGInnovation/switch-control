#pragma once
#include "config-parse.h"
static bool api_config_tool(int fd,const char *name,const ApiJson *j,int args){
    char command[256],a[32],b[32];double value;
    if(!strcmp(name,"get_datetime")||!strcmp(name,"get_auto_time"))return api_args(j,args,"")&&api_run(fd,"clockInfo");
    if(!strcmp(name,"dns_update_status"))return api_args(j,args,"")&&api_run(fd,"dnsStatus");
    if(!strcmp(name,"set_auto_time")){
        if(!api_args(j,args,"|enabled|"))return false;
        int at=aj_get(j,args,"enabled");if(at<0||(j->nodes[at].kind!='t'&&j->nodes[at].kind!='f'))return false;
        snprintf(command,sizeof(command),"clockSync %s",j->nodes[at].kind=='t'?"true":"false");return api_run(fd,command);
    }
    if(!strcmp(name,"set_dns")){
        if(!api_args(j,args,"|automatic||primary||secondary|"))return false;
        int at=aj_get(j,args,"automatic");if(at<0||(j->nodes[at].kind!='t'&&j->nodes[at].kind!='f'))return false;
        bool automatic=j->nodes[at].kind=='t';
        if(automatic){if(aj_get(j,args,"primary")>=0||aj_get(j,args,"secondary")>=0)return false;return api_run(fd,"dnsSet true - -");}
        unsigned char ip[4];strcpy(b,"0.0.0.0");
        if(!api_property(j,args,"primary",a,sizeof(a))||!config_ipv4(a,ip,true))return false;
        if(aj_get(j,args,"secondary")>=0&&!api_property(j,args,"secondary",b,sizeof(b)))return false;
        if(!config_ipv4(b,ip,false))return false;
        snprintf(command,sizeof(command),"dnsSet false %s %s",a,b);return api_run(fd,command);
    }
    if(!api_args(j,args,"|timestamp||year||month||day||hour||minute||second|"))return false;
    const char *fields[]={"year","month","day","hour","minute","second"};
    if(aj_get(j,args,"timestamp")>=0){
        for(unsigned i=0;i<6;i++)if(aj_get(j,args,fields[i])>=0)return false;
        if(!api_num(j,args,"timestamp",&value,946684800,4102444799.0,0)||floor(value)!=value)return false;
        snprintf(command,sizeof(command),"clockSetUtc %llu",(unsigned long long)value);return api_run(fd,command);
    }
    unsigned v[6];for(unsigned i=0;i<6;i++){
        if(aj_get(j,args,fields[i])<0||!api_num(j,args,fields[i],&value,0,i?255:2099,0)||floor(value)!=value)return false;
        v[i]=(unsigned)value;
    }
    if(!config_date(v[0],v[1],v[2],v[3],v[4],v[5]))return false;
    snprintf(command,sizeof(command),"clockSetLocal %u %u %u %u %u %u",v[0],v[1],v[2],v[3],v[4],v[5]);return api_run(fd,command);
}
