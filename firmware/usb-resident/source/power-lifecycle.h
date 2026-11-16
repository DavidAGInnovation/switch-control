#pragma once
/* A distinct PSC registration with FS dependency releases HID and SD handles
 * before sleep, and suppresses filesystem logging during the transition. */
#ifndef NATIVE_CONFIG_SLEEP
#define NATIVE_CONFIG_SLEEP() ((void)0)
#endif
static PscPmModule native_pm;
static Result native_power_result;
static u32 native_sleep_count,native_wake_count;
static void native_power_start(void) {
    native_power_result=pscmInitialize();if(R_FAILED(native_power_result))return;
    const u32 dependencies[]={PscPmModuleId_Fs};
    native_power_result=pscmGetPmModule(&native_pm,(PscPmModuleId)0x5a43,dependencies,1,true);
    power_ready=R_SUCCEEDED(native_power_result);if(!power_ready)pscmExit();
}
static void native_power_poll(void) {
    if(!power_ready||R_FAILED(eventWait(&native_pm.event,0)))return;
    PscPmState next;u32 flags=0;Result rc=pscPmModuleGetRequest(&native_pm,&next,&flags);
    if(R_FAILED(rc)){native_power_result=rc;return;}
    bool sleep=next==PscPmState_ReadySleep||next==PscPmState_ReadySleepCritical||next==PscPmState_ReadyShutdown;
    if(sleep){
        resident_suspended=true;native_sleep_count++;NATIVE_CONFIG_SLEEP();
        input_stop("sleep");
        if(sd_task.active)sd_task_end("sleep-cancelled");
        sd_abort();native_network_transition();
    }else {
        resident_suspended=false;native_wake_count++;
        native_network_transition();
    }
    native_power_result=pscPmModuleAcknowledge(&native_pm,next);
}
static bool native_lifecycle_status(int fd) {
    char out[320];snprintf(out,sizeof(out),"{\"power_registered\":%s,\"suspended\":%s,\"power_result\":%u,\"sleep_events\":%u,\"wake_events\":%u,\"controller_attached\":%s}\n",power_ready?"true":"false",resident_suspended?"true":"false",native_power_result,native_sleep_count,native_wake_count,controller_ready?"true":"false");return sd_reply(fd,out);
}
