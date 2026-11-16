#pragma once
#include "config-parse.h"
/* Only DNS fields are edited. The full saved profile (which includes Wi-Fi
 * credentials) exists only on the stack and is wiped before returning. */
static struct{bool pending,automatic;u64 due;Uuid profile;NifmDnsSetting wanted,previous;char id[33],state[40];Result result;} dns_change;
static bool native_dns_status(int fd){
    char out[200];snprintf(out,sizeof(out),"{\"operation\":\"%s\",\"state\":\"%s\",\"pending\":%s,\"result\":%u}\n",dns_change.id,dns_change.state[0]?dns_change.state:"idle",dns_change.pending?"true":"false",dns_change.result);return sd_reply(fd,out);
}
static bool native_dns_schedule(int fd,const char *line){
    char mode[8],first[32],second[32],extra;
    if(sscanf(line,"dnsSet %7s %31s %31s %c",mode,first,second,&extra)!=3||(strcmp(mode,"true")&&strcmp(mode,"false")))return sd_error(fd,"dns-arguments");
    if(dns_change.pending)return sd_error(fd,"dns-update-busy");
    bool automatic=!strcmp(mode,"true");NifmDnsSetting wanted={0};wanted.is_automatic=automatic;
    if(automatic){if(strcmp(first,"-")||strcmp(second,"-"))return sd_error(fd,"automatic-dns-has-no-addresses");}
    else if(!config_ipv4(first,wanted.primary_dns_server.addr,true)||!config_ipv4(second,wanted.secondary_dns_server.addr,false))return sd_error(fd,"invalid-dns-address");
    NifmNetworkProfileData profile={0};Result rc=nifmGetCurrentNetworkProfile(&profile);
    if(R_FAILED(rc)){memset(&profile,0,sizeof(profile));return sd_error(fd,"network-profile-unavailable");}
    memset(&dns_change,0,sizeof(dns_change));dns_change.profile=profile.uuid;dns_change.previous=profile.ip_setting_data.dns_setting;memset(&profile,0,sizeof(profile));
    dns_change.wanted=wanted;dns_change.automatic=automatic;unsigned char id[16];randomGet(id,sizeof(id));sd_hex(dns_change.id,id,sizeof(id));
    dns_change.pending=true;dns_change.due=armTicksToNs(svcGetSystemTick())/1000000ULL+250;strcpy(dns_change.state,"scheduled");return native_dns_status(fd);
}
static void native_dns_poll(void){
    if(!dns_change.pending||armTicksToNs(svcGetSystemTick())/1000000ULL<dns_change.due)return;
    dns_change.pending=false;NifmNetworkProfileData profile={0};Uuid written={0};
    Result rc=nifmGetCurrentNetworkProfile(&profile);
    if(R_SUCCEEDED(rc)&&memcmp(&profile.uuid,&dns_change.profile,sizeof(Uuid))){strcpy(dns_change.state,"profile-changed");memset(&profile,0,sizeof(profile));return;}
    if(R_SUCCEEDED(rc)&&memcmp(&profile.ip_setting_data.dns_setting,&dns_change.previous,sizeof(NifmDnsSetting))){strcpy(dns_change.state,"dns-settings-changed");memset(&profile,0,sizeof(profile));return;}
    if(R_SUCCEEDED(rc)){
        profile.ip_setting_data.dns_setting.is_automatic=dns_change.automatic;
        if(!dns_change.automatic)profile.ip_setting_data.dns_setting=dns_change.wanted;
        rc=nifmSetNetworkProfile(&profile,&written);
    }
    memset(&profile,0,sizeof(profile));
    bool applied=R_SUCCEEDED(rc);
    if(applied){
        rc=nifmGetNetworkProfile(written,&profile);
        if(R_SUCCEEDED(rc)&&(profile.ip_setting_data.dns_setting.is_automatic!=dns_change.automatic||(!dns_change.automatic&&memcmp(&profile.ip_setting_data.dns_setting,&dns_change.wanted,sizeof(NifmDnsSetting)))))rc=MAKERESULT(Module_Libnx,LibnxError_BadInput);
        memset(&profile,0,sizeof(profile));
    }
    dns_change.result=rc;
    strcpy(dns_change.state,R_SUCCEEDED(rc)?"applied":"failed");
    /* A profile rewrite can invalidate listeners without changing the IP.
     * Rebuild once for this explicit configuration transition; never replay it. */
    if(applied)wifi_current_ip=UINT32_MAX;
}

static void native_dns_cancel_sleep(void){if(dns_change.pending){dns_change.pending=false;strcpy(dns_change.state,"sleep-cancelled");}}
