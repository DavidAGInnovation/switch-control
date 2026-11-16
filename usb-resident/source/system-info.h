#pragma once
#include "utf8-text.h"
/* Read-only system inventory; never returns profile credentials. */
static bool native_network_info(int fd) {
    u32 ip=0,mask=0,gateway=0,first=0,second=0;
    Result rc=nifmGetCurrentIpConfigInfo(&ip,&mask,&gateway,&first,&second);
    if(R_FAILED(rc))return sd_error(fd,"network-unavailable");
    NifmNetworkProfileData profile={0};Result pr=nifmGetCurrentNetworkProfile(&profile);
    NifmInternetConnectionType type=0;NifmInternetConnectionStatus status=0;u32 strength=0;
    Result sr=nifmGetInternetConnectionStatus(&type,&strength,&status);
    u32 values[]={ip,mask,gateway,first,second,0,0};if(R_SUCCEEDED(pr)){memcpy(&values[5],profile.ip_setting_data.dns_setting.primary_dns_server.addr,4);memcpy(&values[6],profile.ip_setting_data.dns_setting.secondary_dns_server.addr,4);}char address[7][20];
    for(unsigned i=0;i<7;i++){const unsigned char *b=(const void*)&values[i];snprintf(address[i],sizeof(address[i]),"%u.%u.%u.%u",b[0],b[1],b[2],b[3]);}
    char out[768];snprintf(out,sizeof(out),"{\"ip\":\"%s\",\"subnet_mask\":\"%s\",\"gateway\":\"%s\",\"primary_dns\":\"%s\",\"secondary_dns\":\"%s\",\"configured_primary_dns\":\"%s\",\"configured_secondary_dns\":\"%s\",\"automatic_dns\":%s,\"connection_type\":%u,\"connection_status\":%u,\"wifi_strength\":%u,\"profile_result\":%u,\"status_result\":%u}\n",address[0],address[1],address[2],address[3],address[4],address[5],address[6],R_SUCCEEDED(pr)?(profile.ip_setting_data.dns_setting.is_automatic?"true":"false"):"null",(unsigned)type,(unsigned)status,strength,pr,sr);
    memset(&profile,0,sizeof(profile));return sd_reply(fd,out);
}
static bool native_titles(int fd,unsigned cursor) {
    if(cursor>100000)return sd_error(fd,"cursor");
    Result rc=nsInitialize();if(R_FAILED(rc))return sd_error(fd,"titles-unavailable");
    Result nc=ncmInitialize();if(R_FAILED(nc)){nsExit();return sd_error(fd,"title-storage-unavailable");}
    NcmContentMetaDatabase databases[3];bool opened[3];const NcmStorageId stores[]={NcmStorageId_SdCard,NcmStorageId_BuiltInUser,NcmStorageId_GameCard};const char *storage_names[]={"sd","internal","gamecard"};
    for(unsigned i=0;i<3;i++)opened[i]=R_SUCCEEDED(ncmOpenContentMetaDatabase(&databases[i],stores[i]));
    NsApplicationRecord records[17]={0};s32 count=0;
    rc=nsListApplicationRecord(records,17,(s32)cursor,&count);
    if(R_FAILED(rc)||count<0||count>17){for(unsigned k=0;k<3;k++)if(opened[k])ncmContentMetaDatabaseClose(&databases[k]);ncmExit();nsExit();return sd_error(fd,"titles-list-failed");}
    bool ok=sd_reply(fd,"{\"titles\":[");static NsApplicationControlData control;
    unsigned emitted=0;
    for(s32 i=0;ok&&i<count&&i<16;i++) {
        NcmContentMetaKey key={0};unsigned storage=0;
        for(;storage<3;storage++)if(opened[storage]&&R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&databases[storage],&key,records[i].application_id))&&key.type==NcmContentMetaType_Application)break;
        if(storage==3)continue;
        if(emitted++)ok=sd_reply(fd,",");
        char prefix[100];snprintf(prefix,sizeof(prefix),"{\"title_id\":\"%016llX\",\"name\":",(unsigned long long)records[i].application_id);ok=ok&&sd_reply(fd,prefix);
        u64 actual=0;char name[513]={0};Result nr=nsGetApplicationControlData(NsApplicationControlSource_Storage,records[i].application_id,&control,sizeof(control),&actual);
        if(R_SUCCEEDED(nr)&&actual>=sizeof(control.nacp)) {
            NacpLanguageEntry *entry=NULL;
            if(R_SUCCEEDED(nacpGetLanguageEntry(&control.nacp,&entry))&&entry){
                size_t length=strnlen(entry->name,sizeof(entry->name));
                if(length<sizeof(entry->name)&&utf8_text_valid(entry->name,length))memcpy(name,entry->name,length);
            }
        }
        char suffix[128];snprintf(suffix,sizeof(suffix),",\"name_available\":%s,\"storage\":\"%s\",\"version\":%u}",name[0]?"true":"false",storage_names[storage],key.version);
        ok=ok&&sd_string(fd,name)&&sd_reply(fd,suffix);SD_COOPERATE();
    }
    for(unsigned k=0;k<3;k++)if(opened[k])ncmContentMetaDatabaseClose(&databases[k]);
    ncmExit();nsExit();char end[80];snprintf(end,sizeof(end),"],\"next\":%lld}\n",count>16?(long long)cursor+16:-1LL);return ok&&sd_reply(fd,end);
}
