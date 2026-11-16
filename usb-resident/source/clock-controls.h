#pragma once
#include "config-parse.h"
#ifndef CLOCK_HOST_TEST
static Result clock_runtime_sync_get(bool *out){u8 value=0;Result rc=serviceDispatchOut(timeGetServiceSession(),100,value);if(R_SUCCEEDED(rc))*out=value!=0;return rc;}
static Result clock_runtime_sync_set(bool enabled){u8 value=enabled;return serviceDispatchIn(timeGetServiceSession(),101,value);}
#endif
static bool clock_result(int fd,Result result,Result rollback){
    char out[180];snprintf(out,sizeof(out),"{\"error\":\"clock-operation\",\"result\":%u,\"rollback_result\":%u}\n",result,rollback);return sd_reply(fd,out);
}
/* Both runtime correction and its persistent preference are updated. */
static Result clock_sync_write(bool enabled){
    Result rc=clock_runtime_sync_set(enabled);
    if(R_SUCCEEDED(rc))rc=setsysSetUserSystemClockAutomaticCorrectionEnabled(enabled);
    return rc;
}
static bool native_clock_info(int fd){
    Result tr=timeInitialize();if(R_FAILED(tr))return clock_result(fd,tr,0);
    Result sr=setsysInitialize();bool runtime=false,saved=false;u64 now=0;
    Result ar=clock_runtime_sync_get(&runtime),pr=sr;
    if(R_SUCCEEDED(sr))pr=setsysIsUserSystemClockAutomaticCorrectionEnabled(&saved);
    Result rc=timeGetCurrentTime(TimeType_UserSystemClock,&now);TimeCalendarTime cal={0};TimeCalendarAdditionalInfo info={0};TimeLocationName zone={0};
    if(R_SUCCEEDED(rc))rc=timeToCalendarTimeWithMyRule(now,&cal,&info);
    Result zr=timeGetDeviceLocationName(&zone);zone.name[sizeof(zone.name)-1]=0;
    if(R_SUCCEEDED(sr)){setsysExit();}timeExit();
    if(R_FAILED(rc))return clock_result(fd,rc,0);
    char out[400];snprintf(out,sizeof(out),"{\"timestamp\":%llu,\"year\":%u,\"month\":%u,\"day\":%u,\"hour\":%u,\"minute\":%u,\"second\":%u,\"utc_offset_seconds\":%d,\"automatic_sync\":%s,\"saved_automatic_sync\":%s,\"sync_result\":%u,\"saved_sync_result\":%u,\"timezone\":",(unsigned long long)now,cal.year,cal.month,cal.day,cal.hour,cal.minute,cal.second,info.offset,R_SUCCEEDED(ar)?(runtime?"true":"false"):"null",R_SUCCEEDED(pr)?(saved?"true":"false"):"null",ar,pr);
    return sd_reply(fd,out)&&(R_SUCCEEDED(zr)?sd_string(fd,zone.name):sd_reply(fd,"null"))&&sd_reply(fd,"}\n");
}
static bool native_clock_set(int fd,const char *line){
    char copy[256];if(strlen(line)>=sizeof(copy))return sd_error(fd,"clock-arguments");strcpy(copy,line);
    char *v[8],*save=NULL;unsigned n=0;for(char *p=strtok_r(copy," ",&save);p;p=strtok_r(NULL," ",&save)){if(n==8)return sd_error(fd,"clock-arguments");v[n++]=p;}
    bool sync=n==2&&!strcmp(v[0],"clockSync");bool utc=n==2&&!strcmp(v[0],"clockSetUtc");bool local=n==7&&!strcmp(v[0],"clockSetLocal");u64 target=0;
    if(!sync&&!utc&&!local)return sd_error(fd,"clock-arguments");
    if(sync&&strcmp(v[1],"true")&&strcmp(v[1],"false"))return sd_error(fd,"clock-boolean");
    if(utc&&!config_uint(v[1],946684800,4102444799ULL,&target))return sd_error(fd,"clock-range");
    TimeCalendarTime requested={0};
    if(local){
        uint64_t fields[6];for(unsigned i=0;i<6;i++)if(!config_uint(v[i+1],0,i?255:2099,&fields[i]))return sd_error(fd,"invalid-date");
        if(!config_date(fields[0],fields[1],fields[2],fields[3],fields[4],fields[5]))return sd_error(fd,"invalid-date");
        requested=(TimeCalendarTime){.year=(u16)fields[0],.month=(u8)fields[1],.day=(u8)fields[2],.hour=(u8)fields[3],.minute=(u8)fields[4],.second=(u8)fields[5]};
    }
    Result rc=timeInitialize();if(R_FAILED(rc))return clock_result(fd,rc,0);
    Result sr=setsysInitialize();if(R_FAILED(sr)){timeExit();return clock_result(fd,sr,0);}
    bool old_runtime=false,old_saved=false;u64 old_local=0,start=armTicksToNs(svcGetSystemTick())/1000000ULL;
    rc=clock_runtime_sync_get(&old_runtime);if(R_SUCCEEDED(rc))rc=setsysIsUserSystemClockAutomaticCorrectionEnabled(&old_saved);
    if(R_SUCCEEDED(rc)&&!sync)rc=timeGetCurrentTime(TimeType_LocalSystemClock,&old_local);
    if(R_SUCCEEDED(rc)&&local){u64 candidates[2]={0};s32 count=0;rc=timeToPosixTimeWithMyRule(&requested,candidates,2,&count);if(R_SUCCEEDED(rc)&&count!=1){setsysExit();timeExit();return sd_error(fd,count>1?"ambiguous-date-use-utc":"nonexistent-local-time");}if(R_SUCCEEDED(rc))target=candidates[0];}
    if(R_FAILED(rc)){setsysExit();timeExit();return clock_result(fd,rc,0);}
    rc=clock_sync_write(sync?!strcmp(v[1],"true"):false);
    bool time_attempted=false;
    if(R_SUCCEEDED(rc)&&!sync){time_attempted=true;rc=timeSetCurrentTime(TimeType_LocalSystemClock,target);}
    if(R_SUCCEEDED(rc)&&!sync){u64 actual=0;rc=timeGetCurrentTime(TimeType_UserSystemClock,&actual);if(R_SUCCEEDED(rc)&&(actual<target||actual-target>2))rc=MAKERESULT(Module_Libnx,LibnxError_BadInput);}
    Result rollback=0;
    if(R_FAILED(rc)){
        if(time_attempted)rollback=timeSetCurrentTime(TimeType_LocalSystemClock,old_local+(armTicksToNs(svcGetSystemTick())/1000000ULL-start)/1000ULL);
        Result r=clock_runtime_sync_set(old_runtime);if(!rollback)rollback=r;
        r=setsysSetUserSystemClockAutomaticCorrectionEnabled(old_saved);if(!rollback)rollback=r;
    }
    setsysExit();timeExit();if(R_FAILED(rc))return clock_result(fd,rc,rollback);
    return sd_reply(fd,"{\"ok\":true}\n");
}
