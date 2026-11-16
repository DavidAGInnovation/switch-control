#pragma once
#include <math.h>
/* Reuse the file API's JSON string emitter and bounded hex helpers. */
static bool system_status(int fd) {
    Result battery_rc=psmInitialize(),brightness_rc=lblInitialize(),audio_rc=audctlInitialize(),settings_rc=setsysInitialize();
    u32 battery=0;PsmChargerType charger=0;Result charge_rc=battery_rc;
    if(R_SUCCEEDED(battery_rc)){charge_rc=psmGetChargerType(&charger);battery_rc=psmGetBatteryChargePercentage(&battery);psmExit();}
    float brightness=0,volume=0;bool automatic=false;Result auto_rc=brightness_rc;
    if(R_SUCCEEDED(brightness_rc)){auto_rc=lblIsAutoBrightnessControlEnabled(&automatic);brightness_rc=lblGetCurrentBrightnessSetting(&brightness);lblExit();}
    if(R_SUCCEEDED(audio_rc)){audio_rc=audctlGetSystemOutputMasterVolume(&volume);audctlExit();}
    ColorSetId theme=0;SetSysDeviceNickName name={0};Result name_rc=settings_rc;
    if(R_SUCCEEDED(settings_rc)){name_rc=setsysGetDeviceNickname(&name);settings_rc=setsysGetColorSetId(&theme);setsysExit();}name.nickname[sizeof(name.nickname)-1]=0;
    char b[48],c[48],l[48],v[48],a[16],t[32];strcpy(b,"null");strcpy(c,"null");strcpy(l,"null");strcpy(v,"null");strcpy(a,"null");strcpy(t,"null");
    if(R_SUCCEEDED(battery_rc))snprintf(b,sizeof(b),"%u",battery);
    if(R_SUCCEEDED(charge_rc))snprintf(c,sizeof(c),"%u",(unsigned)charger);
    if(R_SUCCEEDED(brightness_rc)&&isfinite(brightness))snprintf(l,sizeof(l),"%.6f",brightness);
    if(R_SUCCEEDED(audio_rc)&&isfinite(volume))snprintf(v,sizeof(v),"%.6f",volume);
    if(R_SUCCEEDED(auto_rc))strcpy(a,automatic?"true":"false");
    if(R_SUCCEEDED(settings_rc))snprintf(t,sizeof(t),"\"%s\"",theme==ColorSetId_Dark?"dark":"light");
    u32 ip=0;Result ip_rc=nifmGetCurrentIpAddress(&ip);struct in_addr address={.s_addr=ip};char out[1024];
    int n=snprintf(out,sizeof(out),"{\"version\":\"0.5.1\",\"firmware\":\"%u.%u.%u\",\"uptime_ms\":%llu,\"controller_attached\":%s,\"memory_read_only\":true,\"sd_files\":true,\"analog_sticks\":true,\"battery_percent\":%s,\"charger_type\":%s,\"brightness\":%s,\"auto_brightness\":%s,\"volume\":%s,\"theme\":%s,\"ip\":\"%s\",\"errors\":{\"battery\":%u,\"charger\":%u,\"brightness\":%u,\"auto_brightness\":%u,\"volume\":%u,\"theme\":%u,\"nickname\":%u},\"nickname\":",HOSVER_MAJOR(hosversionGet()),HOSVER_MINOR(hosversionGet()),HOSVER_MICRO(hosversionGet()),(unsigned long long)(armTicksToNs(svcGetSystemTick()-reader_started)/1000000),controller_ready?"true":"false",b,c,l,a,v,t,R_SUCCEEDED(ip_rc)?inet_ntoa(address):"",battery_rc,charge_rc,brightness_rc,auto_rc,audio_rc,settings_rc,name_rc);
    if(n<0||(size_t)n>=sizeof(out))return false;
    return sd_reply(fd,out)&&(R_SUCCEEDED(name_rc)?sd_string(fd,name.nickname):sd_reply(fd,"null"))&&sd_reply(fd,"}\n");
}
static bool system_set(int fd,const char *line) {
    char key[32],value[300],extra;if(sscanf(line,"systemSet %31s %299s %c",key,value,&extra)!=2)return sd_error(fd,"arguments");
    Result rc=MAKERESULT(Module_Libnx,LibnxError_BadInput);
    if(!strcmp(key,"brightness")||!strcmp(key,"volume")) {
        char *end;errno=0;float x=strtof(value,&end);if(errno||*end||!isfinite(x)||x<0||x>1)return sd_error(fd,"range");
        if(!strcmp(key,"brightness")){rc=lblInitialize();if(R_SUCCEEDED(rc)){rc=lblSetCurrentBrightnessSetting(x);if(R_SUCCEEDED(rc))rc=lblApplyCurrentBrightnessSettingToBacklight();lblExit();}}
        else {rc=audctlInitialize();if(R_SUCCEEDED(rc)){rc=audctlSetSystemOutputMasterVolume(x);audctlExit();}}
    }else if(!strcmp(key,"auto_brightness")) {
        if(strcmp(value,"true")&&strcmp(value,"false"))return sd_error(fd,"boolean");
        rc=lblInitialize();if(R_SUCCEEDED(rc)){rc=!strcmp(value,"true")?lblEnableAutoBrightnessControl():lblDisableAutoBrightnessControl();lblExit();}
    }else if(!strcmp(key,"theme")) {
        if(strcmp(value,"light")&&strcmp(value,"dark"))return sd_error(fd,"theme");
        rc=setsysInitialize();if(R_SUCCEEDED(rc)){rc=setsysSetColorSetId(!strcmp(value,"dark")?ColorSetId_Dark:ColorSetId_Light);setsysExit();}
    }else if(!strcmp(key,"nickname")) {
        size_t n=strlen(value);SetSysDeviceNickName nickname={0};if(!n||(n&1)||n/2>=sizeof(nickname.nickname))return sd_error(fd,"nickname");
        for(size_t i=0;i<n/2;i++){int a=sd_nibble(value[2*i]),b=sd_nibble(value[2*i+1]);if(a<0||b<0)return sd_error(fd,"nickname");nickname.nickname[i]=(char)((a<<4)|b);if((unsigned char)nickname.nickname[i]<32||nickname.nickname[i]==127)return sd_error(fd,"nickname");}
        rc=setsysInitialize();if(R_SUCCEEDED(rc)){rc=setsysSetDeviceNickname(&nickname);setsysExit();}
    }else return sd_error(fd,"unknown-setting");
    if(R_FAILED(rc)){char out[96];snprintf(out,sizeof(out),"{\"error\":\"native-setting\",\"result\":%u}\n",rc);return sd_reply(fd,out);}return sd_reply(fd,"{\"ok\":true}\n");
}
