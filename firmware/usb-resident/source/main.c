// USB/Wi-Fi controller + dmnt reader. No independent debugger or game writes.
#define NX_SERVICE_ASSUME_NON_DOMAIN
#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/stat.h>
#include "wifi-recovery.h"
#include "stick.h"
#ifdef ZA_WIFI_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/platform_time.h>
#include <psa/crypto.h>
static bool wifi_tls_write_all(int fd,const char *data,size_t size);
static bool wifi_tls_read_all(int fd,void *data,size_t size);
static void sd_abort(void);
#ifdef CONTROL_NATIVE_HTTP
static bool api_info(int fd);
#endif
#endif

u32 __nx_applet_type = AppletType_None;
TimeServiceType __nx_time_service_type = TimeServiceType_Menu;
static Result startup_rc;
static bool resident_suspended, power_ready;
static void input_poll(void);
static void native_network_transition(void);
static void native_network_resume(void);
static bool native_lifecycle_status(int fd);
static u32 wifi_current_ip=UINT32_MAX;
static void resident_log(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void resident_log(const char *fmt, ...) {
#ifdef ZA_ISOLATED_TEST
    if(resident_suspended)return;
    FILE *f=fopen("sdmc:/config/za-usb-control/wifi-test-status.log","a");
#else
    if(resident_suspended)return;
    FILE *f=fopen("sdmc:/config/za-usb-control/status.log","a");
#endif
    if(!f)return;
    va_list ap;va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);fflush(f);fclose(f);
}
#define printf resident_log
#include "../../reader/source/main.c"
#undef printf

static HiddbgHdlsSessionId session;
static HiddbgHdlsHandle controller;
static HiddbgHdlsState state;
static bool controller_ready, hiddbg_ready;
static bool controller_owned_by_wifi;
static Result controller_cleanup_error;
static u32 controller_stage;
static Result controller_stage_result;
static u8 controller_work[0x1000] __attribute__((aligned(0x1000)));
static u8 jpeg[0x80000] __attribute__((aligned(0x1000)));
static HiddbgHdlsStateList controller_inventory;
static u64 last_control;
static Result detach_controller(void);

#ifdef ZA_ISOLATED_TEST
// The HID ownership guard alone still permits multiple full test services to
// coexist. Refuse an isolated test if a peer is running or inspection fails.
static Result isolated_service_guard(void) {
    static const u64 ids[] = {0x430000000000000BULL, 0x430000000000000CULL,
                              0x430000000000000DULL, 0x430000000000000EULL};
    u64 self = 0;
    Result rc = svcGetProcessId(&self, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc)) return rc;
    rc = pmdmntInitialize();
    if (R_FAILED(rc)) return rc;
    bool seen_self = false;
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); ++i) {
        u64 pid = 0;
        rc = pmdmntGetProcessId(&pid, ids[i]);
        if (rc == 0x20f) continue;
        if (R_FAILED(rc)) break;
        if (pid != self) { rc = MAKERESULT(Module_Libnx, LibnxError_BadInput); break; }
        seen_self = true;
    }
    pmdmntExit();
    if (rc != 0 && rc != 0x20f) return rc;
    return seen_self ? 0 : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
}

// Record the surrounding devices without sending input. This distinguishes an
// inherited virtual device from one created by this session.
static void log_controller_inventory(const char *phase) {
    HiddbgHdlsStateList list={0};
    Result rc=hiddbgDumpHdlsStates(session,&list);
    resident_log("HID inventory %s rc=%08X count=%d target=%016" PRIx64 "\n",
        phase,rc,list.total_entries,controller.handle);
    if(R_FAILED(rc)||list.total_entries<0||list.total_entries>16)return;
    for(int i=0;i<list.total_entries;i++)
        resident_log("HID entry %s %d handle=%016" PRIx64 " type=%u interface=%u\n",
            phase,i,list.entries[i].handle.handle,list.entries[i].device.deviceType,
            list.entries[i].device.npadInterfaceType);
}
#endif

// A session can outlive its virtual device. In that case the old handle must
// never be sent to HID: release the work buffer without attempting a detach.
static Result release_missing_controller(void) {
    controller_ready=false;
    Result rc=hiddbgReleaseHdlsWorkBuffer(session);
    if(R_FAILED(rc)){controller_cleanup_error=rc;return rc;}
    hiddbgExit();hiddbg_ready=false;
    return 0;
}

// Never attach a second instance of this service's virtual controller. The
// test service uses a distinct program ID and requires the boot service to be
// stopped; the boot service likewise refuses input while the test is running.
static Result controller_peer_guard(void) {
#if defined(ZA_TLS_TEST) || defined(ZA_WIFI_TLS_PROGRAM_D)
    // TLS test program D may run beside the installed C service for
    // read-only checks, but must not attach another virtual controller.
    Result test_rc=pmdmntInitialize();if(R_FAILED(test_rc))return test_rc;
    u64 test_pid=0;test_rc=pmdmntGetProcessId(&test_pid,0x430000000000000CULL);pmdmntExit();
    if(R_SUCCEEDED(test_rc))return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    if(test_rc!=0x20f)return test_rc;
#endif
#if defined(ZA_ISOLATED_TEST) || defined(ZA_WIFI_ALT_PROGRAM) || defined(ZA_WIFI_TLS_PROGRAM_D)
    const u64 peer=0x430000000000000BULL;
#else
    const u64 peer=0x430000000000000CULL;
#endif
    Result rc=pmdmntInitialize();if(R_FAILED(rc))return rc;
    u64 pid=0;rc=pmdmntGetProcessId(&pid,peer);pmdmntExit();
    if(rc==0x20f)return 0; // PM: program not running.
    if(R_SUCCEEDED(rc))return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    return rc;
}

void __libnx_initheap(void) {
    static u8 heap[0x300000];
    extern void *fake_heap_start,*fake_heap_end;
    fake_heap_start=heap;fake_heap_end=heap+sizeof(heap);
}
void __appInit(void) {
    startup_rc=smInitialize();if(R_FAILED(startup_rc))return;
    Result rc=setsysInitialize();
    if(R_SUCCEEDED(rc)) {SetSysFirmwareVersion fw;
        if(R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))hosversionSet(MAKEHOSVERSION(fw.major,fw.minor,fw.micro));
        setsysExit();}
    rc=fsInitialize();if(R_FAILED(rc)){startup_rc=rc;return;}
    rc=fsdevMountSdmc();if(R_FAILED(rc)){startup_rc=rc;return;}
    mkdir("sdmc:/config/za-usb-control",0777);
#ifdef ZA_ISOLATED_TEST
    FILE *f=fopen("sdmc:/config/za-usb-control/wifi-test-status.log","w");if(f)fclose(f);
#else
    FILE *f=fopen("sdmc:/config/za-usb-control/status.log","w");if(f)fclose(f);
#endif
}
void __appExit(void) {
#ifdef ZA_WIFI_TLS
    sd_abort();
#endif
    if(controller_ready) (void)detach_controller();
    if(hiddbg_ready)hiddbgExit();
    if(serviceIsActive(&dmnt))serviceClose(&dmnt);
    usbCommsExit();fsdevUnmountAll();fsExit();smExit();
}
static Result ensure_controller(int fd) {
    if(R_FAILED(controller_cleanup_error))return controller_cleanup_error;
    // The two transports must never manipulate one another's held state.
    if(controller_ready && controller_owned_by_wifi!=(fd>=0))
        return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    controller_stage=1;
    Result peer=controller_peer_guard();
    controller_stage_result=peer;
    if(R_FAILED(peer)) {
        if(controller_ready)(void)detach_controller();
        return peer;
    }
    if(controller_ready){
        bool attached=false;
        controller_stage=7;
        Result rc=hiddbgIsHdlsVirtualDeviceAttached(session,controller,&attached);
        controller_stage_result=rc;
        if(R_FAILED(rc)){controller_cleanup_error=rc;return rc;}
        if(!attached){
            rc=release_missing_controller();
            controller_stage_result=R_FAILED(rc)?rc:MAKERESULT(Module_Libnx,LibnxError_NotInitialized);
            return controller_stage_result;
        }
        controller_stage=6;controller_stage_result=0;return 0;
    }
    controller_stage=2;
    Result rc=hiddbgInitialize();controller_stage_result=rc;if(R_FAILED(rc))return rc;hiddbg_ready=true;
    HiddbgHdlsDeviceInfo info={0};
    info.deviceType=HidDeviceType_FullKey3;info.npadInterfaceType=HidNpadInterfaceType_Bluetooth;
    info.singleColorBody=RGBA8_MAXALPHA(0,0,0);info.singleColorButtons=RGBA8_MAXALPHA(255,255,255);
    info.colorLeftGrip=RGBA8_MAXALPHA(0,0,255);info.colorRightGrip=RGBA8_MAXALPHA(0,255,0);
    controller_stage=3;
    rc=hiddbgAttachHdlsWorkBuffer(&session,controller_work,sizeof(controller_work));controller_stage_result=rc;
    if(R_FAILED(rc)){hiddbgExit();hiddbg_ready=false;return rc;}
#ifdef ZA_ISOLATED_TEST
    log_controller_inventory("before-attach");
#endif
    controller_stage=4;
    rc=hiddbgAttachHdlsVirtualDevice(&controller,&info);controller_stage_result=rc;
    if(R_FAILED(rc)){
        Result cleanup=hiddbgReleaseHdlsWorkBuffer(session);
        if(R_FAILED(cleanup)){controller_cleanup_error=cleanup;return cleanup;}
        hiddbgExit();hiddbg_ready=false;return rc;
    }
    controller_owned_by_wifi=fd>=0;
#ifdef ZA_ISOLATED_TEST
    log_controller_inventory("after-attach");
#endif
    memset(&state,0,sizeof(state));state.battery_level=4;
    controller_stage=5;
    rc=hiddbgSetHdlsState(controller,&state);controller_stage_result=rc;
#ifdef ZA_ISOLATED_TEST
    log_controller_inventory("after-neutral");
#endif
    if(R_FAILED(rc)){
        controller_ready=true;
        Result cleanup=detach_controller();
        return R_FAILED(cleanup)?cleanup:rc;
    }
    controller_ready=true;
    // Attach and initial neutral state can succeed even if HID has already
    // invalidated the returned handle. Never issue a button to that handle.
    bool attached=false;
    controller_stage=7;
    rc=hiddbgIsHdlsVirtualDeviceAttached(session,controller,&attached);
    controller_stage_result=rc;
    if(R_FAILED(rc)){controller_cleanup_error=rc;return rc;}
    if(!attached){
        rc=release_missing_controller();
        controller_stage_result=R_FAILED(rc)?rc:MAKERESULT(Module_Libnx,LibnxError_NotInitialized);
        return controller_stage_result;
    }
    controller_stage=6;controller_stage_result=0;
    last_control=svcGetSystemTick();return 0;
}
static Result neutral(void) {
    if(!controller_ready)return 0;
    state.buttons=0;state.analog_stick_l=(HidAnalogStickState){0};state.analog_stick_r=(HidAnalogStickState){0};
    last_control=svcGetSystemTick();return hiddbgSetHdlsState(controller,&state);
}
static Result detach_controller(void) {
    if(!controller_ready)return controller_cleanup_error;
    bool attached=false;
    Result check=hiddbgIsHdlsVirtualDeviceAttached(session,controller,&attached);
    if(R_FAILED(check)){controller_cleanup_error=check;return check;}
    if(!attached)return release_missing_controller();
    Result neutral_rc=neutral();
    Result rc=hiddbgDetachHdlsVirtualDevice(controller);
    if(R_FAILED(rc)){controller_cleanup_error=rc;return rc;}
    controller_ready=false;
    rc=hiddbgReleaseHdlsWorkBuffer(session);
    if(R_FAILED(rc)){controller_cleanup_error=rc;return rc;}
    hiddbgExit();hiddbg_ready=false;
    return neutral_rc;
}
// A Wi-Fi input is one atomic request. Release its HID session before
// acknowledging success, so an idle/disconnected client does not leave a
// virtual controller behind. USB may deliberately hold input across requests.
static Result finish_controller_input(int fd, Result input_rc) {
    if(fd>=0 || R_FAILED(input_rc)) {
        Result cleanup=detach_controller();
        if(R_SUCCEEDED(input_rc))return cleanup;
    }
    return input_rc;
}
static bool button(const char *s,u64 *mask) {
    static const struct {const char *name;u64 mask;} values[]={
        {"A",HidNpadButton_A},{"B",HidNpadButton_B},{"X",HidNpadButton_X},{"Y",HidNpadButton_Y},
        {"L",HidNpadButton_L},{"R",HidNpadButton_R},{"ZL",HidNpadButton_ZL},{"ZR",HidNpadButton_ZR},
        {"PLUS",HidNpadButton_Plus},{"MINUS",HidNpadButton_Minus},{"UP",HidNpadButton_Up},{"DOWN",HidNpadButton_Down},{"LEFT",HidNpadButton_Left},{"RIGHT",HidNpadButton_Right},{"DUP",HidNpadButton_Up},
        {"DDOWN",HidNpadButton_Down},{"DD",HidNpadButton_Down},{"DLEFT",HidNpadButton_Left},
        {"DRIGHT",HidNpadButton_Right},{"LSTICK",HidNpadButton_StickL},{"RSTICK",HidNpadButton_StickR},
        {"HOME",HiddbgNpadButton_Home},{"CAPTURE",HiddbgNpadButton_Capture}};
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i)if(!strcmp(s,values[i].name)){*mask=values[i].mask;return true;}
    return false;
}
static bool reply(int fd, Result rc) {return R_FAILED(rc)?error_reply(fd,rc):send_all(fd,"OK\n",3);}
// Atmosphere's cheat service has a small shared session limit. Hold one only
// while serving this request, including native-error and transport-failure
// paths; the cheat manager's selected process is independent of this handle.
static bool scoped_reader_command(int fd,const char *line) {
    if(serviceIsActive(&dmnt))serviceClose(&dmnt);
    Result rc=smGetService(&dmnt,"dmnt:cht");
    if(R_FAILED(rc))return error_reply(fd,rc);
    bool sent=command(fd,line);
    serviceClose(&dmnt);
    return sent;
}
// Release the capture service before sending a potentially large response, or
// returning an error, so it is not retained across HOME/applet transitions.
static bool scoped_screenshot(int fd) {
    Result rc=capsscInitialize();
    if(R_FAILED(rc))return error_reply(fd,rc);
    u64 size=0;
    rc=capsscCaptureJpegScreenShot(&size,jpeg,sizeof(jpeg),ViLayerStack_Screenshot,1000000000ULL);
    capsscExit();
    if(R_FAILED(rc))return error_reply(fd,rc);
    if(size>sizeof(jpeg))return send_all(fd,"ERR invalid-screenshot-size\n",28);
    char header[48];int n=snprintf(header,sizeof(header),"JPEG %" PRIu64 "\n",size);
    return send_all(fd,header,(size_t)n)&&send_all(fd,(const char*)jpeg,(size_t)size);
}
#ifdef ZA_WIFI_TLS
static bool sd_send(int fd,const void *data,size_t n){return send_all(fd,data,n);}
static bool sd_receive(int fd,void *data,size_t n){return wifi_tls_read_all(fd,data,n);}
static void sd_random(void *data,size_t n){randomGet(data,n);}
static bool sd_mutation_allowed(const char *path){
    if(strncasecmp(path,"/atmosphere/",12))return true;
    Result rc=pmdmntInitialize();if(R_FAILED(rc))return false;u64 pid=0;
    rc=pmdmntGetApplicationProcessId(&pid);pmdmntExit();return rc==0x20f;
}
#define SD_COOPERATE() input_poll()
#include "sd-files.h"
#include "system-controls.h"
#include "discovery.h"
#endif

#ifdef ZA_WIFI_TLS
#include "input-jobs.h"
#define INPUT_RUNNING (input_job.active)
static uint64_t input_now(void){return armTicksToNs(svcGetSystemTick())/1000000ULL;}
static bool input_available(void){return power_ready&&!resident_suspended&&!controller_ready;}
static bool input_apply(const InputStep *p){
    const char *names[]={"A","B","X","Y","L","R","ZL","ZR","PLUS","MINUS","UP","DOWN","LEFT","RIGHT","LSTICK","RSTICK","HOME","CAPTURE"};u64 allowed=0;
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){u64 bit=0;if(button(names[i],&bit))allowed|=bit;}
    if(p->buttons&~allowed)return false;
    if(resident_suspended||R_FAILED(ensure_controller(1)))return false;
    state.buttons=p->buttons;state.analog_stick_l=(HidAnalogStickState){p->lx,p->ly};state.analog_stick_r=(HidAnalogStickState){p->rx,p->ry};last_control=svcGetSystemTick();return R_SUCCEEDED(hiddbgSetHdlsState(controller,&state));
}
static bool input_release(void){return R_SUCCEEDED(detach_controller());}
static void input_random(void *p,size_t n){randomGet(p,n);}
static bool input_reply(int fd,const char *text){return sd_reply(fd,text);}
static bool input_error(int fd,const char *text){return sd_error(fd,text);}
#endif

#ifdef ZA_WIFI_TLS
#include "system-info.h"
#include "network-config.h"
#include "clock-controls.h"
#else
#define INPUT_RUNNING false
#endif
static bool dispatch(int fd, const char *line) {
#ifdef ZA_WIFI_TLS
    if(!strcmp(line,"dnsStatus"))return native_dns_status(fd);
    if(!strncmp(line,"dnsSet ",7))return native_dns_schedule(fd,line);
    if(!strcmp(line,"clockInfo"))return native_clock_info(fd);
    if(!strncmp(line,"clockSync ",10)||!strncmp(line,"clockSet",8))return native_clock_set(fd,line);
    if(!strcmp(line,"networkInfo"))return native_network_info(fd);
    if(!strcmp(line,"lifecycleInfo"))return native_lifecycle_status(fd);
    if(!strncmp(line,"listTitles ",11)){char *end;unsigned long n=strtoul(line+11,&end,10);if(*end||n>100000)return sd_error(fd,"cursor");return native_titles(fd,(unsigned)n);}
    if(!strncmp(line,"input",5))return input_command(fd,line);
    if(input_job.active&&(!strncmp(line,"stick ",6)||!strncmp(line,"click",5)||!strncmp(line,"press ",6)||!strncmp(line,"release ",8)))return sd_error(fd,"input-busy");
    if(!strcmp(line,"detachController"))input_stop("cancelled");
#endif
#ifdef CONTROL_NATIVE_HTTP
    if(!strcmp(line,"getApiInfo"))return api_info(fd);
#endif
    if(!strncmp(line,"fs",2)) {
#ifdef ZA_WIFI_TLS
        if(fd>=0)return sd_files_command(fd,line);
#endif
        return send_all(fd,"ERR files-require-TLS\n",22);
    }

#ifdef ZA_WIFI_TLS
    if(!strcmp(line,"systemStatus"))return system_status(fd);
    if(!strncmp(line,"systemSet ",10))return system_set(fd,line);
#endif
    if(!strncmp(line,"stick ",6)) {
        char side;int x,y;unsigned ms;
        if(!stick_parse(line,&side,&x,&y,&ms))return send_all(fd,"ERR invalid-stick\n",18);
        Result rc=ensure_controller(fd);if(R_FAILED(rc))return reply(fd,rc);
        HidAnalogStickState *target=side=='L'?&state.analog_stick_l:&state.analog_stick_r;
        target->x=x;target->y=y;rc=hiddbgSetHdlsState(controller,&state);last_control=svcGetSystemTick();
        if(R_SUCCEEDED(rc))svcSleepThread((u64)ms*1000000ULL);
        Result cleanup=neutral();if(R_SUCCEEDED(rc))rc=cleanup;
        return reply(fd,finish_controller_input(fd,rc));
    }
    char op[32],arg[224],extra;u64 mask;
    if(!strcmp(line,"getTransport")) {const char *s=fd<0?"USB ZAU1 frame=4096 payload=4092 resident-read-only\n":"TCP ZAW1 authenticated resident-read-only\n";return send_all(fd,s,strlen(s));}
    if(!strcmp(line,"getVersion"))return send_all(fd,"switch-control-0.5.0\n",21);
    if(!strcmp(line,"getReaderStatus")) {
        char s[240];int n=snprintf(s,sizeof(s),"RESIDENT uptime_ms=%" PRIu64 " firmware=%u.%u.%u controller=%u stage=%u stage_rc=%08X cleanup=%08X startup=%08X memory-read-only=1\n",
            armTicksToNs(svcGetSystemTick()-reader_started)/1000000,
            HOSVER_MAJOR(hosversionGet()),HOSVER_MINOR(hosversionGet()),HOSVER_MICRO(hosversionGet()),controller_ready,controller_stage,controller_stage_result,controller_cleanup_error,startup_rc);
        return send_all(fd,s,(size_t)n);
    }
    if(!strcmp(line,"neutral")) {
        if(controller_ready && controller_owned_by_wifi!=(fd>=0))
            return reply(fd,MAKERESULT(Module_Libnx,LibnxError_BadInput));
        return reply(fd,neutral());
    }
#ifdef ZA_ISOLATED_TEST
    if(!strcmp(line,"prepareController"))return reply(fd,ensure_controller(fd));
    // Inventory the idle HID state without creating a virtual controller.
    if(!strcmp(line,"getIdleInventory")) {
        if(controller_ready||hiddbg_ready||R_FAILED(controller_cleanup_error))
            return send_all(fd,"ERR controller-not-idle\n",24);
        Result rc=hiddbgInitialize();if(R_FAILED(rc))return error_reply(fd,rc);
        hiddbg_ready=true;
        rc=hiddbgAttachHdlsWorkBuffer(&session,controller_work,sizeof(controller_work));
        if(R_FAILED(rc)){hiddbgExit();hiddbg_ready=false;return error_reply(fd,rc);}
        memset(&controller_inventory,0,sizeof(controller_inventory));
        rc=hiddbgDumpHdlsStates(session,&controller_inventory);
        Result cleanup=hiddbgReleaseHdlsWorkBuffer(session);
        if(R_FAILED(cleanup)){controller_cleanup_error=cleanup;return error_reply(fd,cleanup);}
        hiddbgExit();hiddbg_ready=false;
        if(R_FAILED(rc))return error_reply(fd,rc);
        if(controller_inventory.total_entries<0||controller_inventory.total_entries>16)
            return send_all(fd,"ERR invalid-inventory\n",22);
        char out[1024];size_t used=0;
        int n=snprintf(out,sizeof(out),"count=%d\n",controller_inventory.total_entries);
        if(n<0||(size_t)n>=sizeof(out))return send_all(fd,"ERR inventory-overflow\n",23);
        used=(size_t)n;
        for(int i=0;i<controller_inventory.total_entries;i++) {
            const HiddbgHdlsStateListEntry *x=&controller_inventory.entries[i];
            n=snprintf(out+used,sizeof(out)-used,"%d handle=%016" PRIx64 " type=%u interface=%u\n",
                i,x->handle.handle,x->device.deviceType,x->device.npadInterfaceType);
            if(n<0||(size_t)n>=sizeof(out)-used)return send_all(fd,"ERR inventory-overflow\n",23);
            used+=(size_t)n;
        }
        return send_all(fd,out,used);
    }
    // Test-only: create a stale handle without sending input. The device is
    // first neutralized and detached; retain the cached handle solely to test
    // that the next request detects its absence and releases the session.
    if(!strcmp(line,"simulateControllerLoss")) {
        if(!controller_ready)return send_all(fd,"ERR controller-not-attached\n",28);
        Result rc=neutral();if(R_SUCCEEDED(rc))rc=hiddbgDetachHdlsVirtualDevice(controller);
        return reply(fd,rc);
    }
    if(!strcmp(line,"getControllerHealth")) {
        if(!controller_ready)return send_all(fd,"ERR controller-not-attached\n",28);
        bool attached=false;
        Result rc=hiddbgIsHdlsVirtualDeviceAttached(session,controller,&attached);
        if(R_FAILED(rc))return error_reply(fd,rc);
        char out[128];int n=snprintf(out,sizeof(out),"attached=%u handle=%016" PRIx64 " session=%016" PRIx64 " cleanup=%08X\n",
            attached,controller.handle,session.id,controller_cleanup_error);
        return send_all(fd,out,(size_t)n);
    }
#endif
    if(!strcmp(line,"getControllerInventory")) {
        if(!controller_ready)return send_all(fd,"ERR controller-not-attached\n",28);
#ifdef ZA_ISOLATED_TEST
        bool before=false,after=false;
        u64 start=svcGetSystemTick();
        Result health_rc=hiddbgIsHdlsVirtualDeviceAttached(session,controller,&before);
        if(R_FAILED(health_rc))return error_reply(fd,health_rc);
#endif
        memset(&controller_inventory,0,sizeof(controller_inventory));
        Result rc=hiddbgDumpHdlsStates(session,&controller_inventory);
        if(R_FAILED(rc))return error_reply(fd,rc);
        if(controller_inventory.total_entries<0||controller_inventory.total_entries>16)
            return send_all(fd,"ERR invalid-inventory\n",22);
        char out[2048];size_t used=0;
#ifdef ZA_ISOLATED_TEST
        health_rc=hiddbgIsHdlsVirtualDeviceAttached(session,controller,&after);
        if(R_FAILED(health_rc))return error_reply(fd,health_rc);
        int header=snprintf(out,sizeof(out),"handle=%016" PRIx64 " session=%016" PRIx64 " before=%u after=%u count=%d elapsed_ns=%" PRIu64 "\n",
            controller.handle,session.id,before,after,controller_inventory.total_entries,
            armTicksToNs(svcGetSystemTick()-start));
        if(header<0||(size_t)header>=sizeof(out))return send_all(fd,"ERR inventory-overflow\n",23);
        used=(size_t)header;
#endif
        for(int i=0;i<controller_inventory.total_entries;i++) {
            const HiddbgHdlsStateListEntry *x=&controller_inventory.entries[i];
            int n=snprintf(out+used,sizeof(out)-used,"%d handle=%016" PRIx64 " type=%u interface=%u flags=%x battery=%u buttons=%016" PRIx64 " attr=%x\n",
                i,x->handle.handle,x->device.deviceType,x->device.npadInterfaceType,
                x->state.flags,x->state.battery_level,x->state.buttons,x->state.attribute);
            if(n<0||(size_t)n>=sizeof(out)-used)return send_all(fd,"ERR inventory-overflow\n",23);
            used+=(size_t)n;
        }
        if(!used)return send_all(fd,"EMPTY\n",6);
        return send_all(fd,out,used);
    }
    if(!strcmp(line,"detachController")) {
        if(controller_ready && controller_owned_by_wifi!=(fd>=0))
            return reply(fd,MAKERESULT(Module_Libnx,LibnxError_BadInput));
        return reply(fd,detach_controller());
    }
    if(sscanf(line,"%31s %223s %c",op,arg,&extra)==2 &&
        (!strcmp(op,"click") || !strcmp(op,"press") || !strcmp(op,"release"))) {
        if(!button(arg,&mask))return send_all(fd,"ERR invalid-button\n",19);
        Result rc=ensure_controller(fd);if(R_FAILED(rc))return reply(fd,rc);
        if(!strcmp(op,"release"))state.buttons&=~mask;else state.buttons|=mask;
        rc=hiddbgSetHdlsState(controller,&state);last_control=svcGetSystemTick();
        if(!strcmp(op,"click")&&R_SUCCEEDED(rc)){svcSleepThread(50000000);state.buttons&=~mask;
            Result r=hiddbgSetHdlsState(controller,&state);if(R_SUCCEEDED(rc))rc=r;}
        return reply(fd,finish_controller_input(fd,rc));
    }
    if(!strncmp(line,"clickSeq ",9)) {
        char sequence[224];if(strlen(line+9)>=sizeof(sequence))return send_all(fd,"ERR invalid-sequence\n",21);
        strcpy(sequence,line+9);
        struct Step {char action;u64 value;} steps[32];size_t count=0;unsigned total=0;char *save=NULL;
        for(char *s=strtok_r(sequence,",",&save);s;s=strtok_r(NULL,",",&save)) {
            if(count==32)return send_all(fd,"ERR invalid-sequence\n",21);
            if(*s=='W'){char *end;unsigned long v=strtoul(s+1,&end,10);
                if(!s[1] || *end || v>1000 || total+v>5000)return send_all(fd,"ERR invalid-sequence\n",21);
                steps[count++]=(struct Step){'W',v};total+=(unsigned)v;
            }else if((*s=='+' || *s=='-') && button(s+1,&mask))steps[count++]=(struct Step){*s,mask};
            else return send_all(fd,"ERR invalid-sequence\n",21);
        }
        if(!count)return send_all(fd,"ERR invalid-sequence\n",21);
        Result rc=ensure_controller(fd);if(R_FAILED(rc))return reply(fd,rc);
        for(size_t i=0;i<count && R_SUCCEEDED(rc);++i) {
            if(steps[i].action=='W')svcSleepThread(steps[i].value*1000000ULL);
            else {if(steps[i].action=='+')state.buttons|=steps[i].value;else state.buttons&=~steps[i].value;
                rc=hiddbgSetHdlsState(controller,&state);}
        }
        if(R_SUCCEEDED(rc))rc=neutral();
        return reply(fd,finish_controller_input(fd,rc));
    }
    if(!strcmp(line,"pixelPeek")) {
        return scoped_screenshot(fd);
    }
    if(!strcmp(line,"getPausedCapture"))return send_all(fd,"ERR use-pixelPeek\n",18);
    // Generic reads are bounded; build-specific object operations are gated.
    // The explicit openForeground command asks dmnt to attach if needed.
    return scoped_reader_command(fd,line);
}

// TCP is disabled unless a 32-byte private key is provisioned on the SD card.
// A fresh nonce authenticates each single command; commands cannot be replayed.
static int wifi_listener = -1;
static u8 wifi_key[32];
static WifiRecovery wifi_recovery;
static bool wifi_nifm_ready;
static u64 wifi_health_tick;
static void wifi_close_peers(void);
#ifdef ZA_WIFI_TLS
static mbedtls_ssl_config wifi_tls_config;
static mbedtls_entropy_context wifi_tls_entropy;
static mbedtls_ctr_drbg_context wifi_tls_rng;
static u8 wifi_tls_psk[32];
static const int wifi_tls_ciphers[]={MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,0};

// libnx's OS-seeded ChaCha generator fails closed if kernel seeding fails.
int mbedtls_hardware_poll(void *ctx,unsigned char *out,size_t len,size_t *olen) {
    (void)ctx;randomGet(out,len);*olen=len;return 0;
}
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(armTicksToNs(svcGetSystemTick())/1000000ULL);
}
static bool wifi_tls_init(void) {
#ifdef ZA_TLS_TEST
    const char *path="sdmc:/config/za-usb-control/tls-test.key";
#else
    const char *path="sdmc:/config/za-usb-control/tls.key";
#endif
    FILE *f=fopen(path,"rb");if(!f){resident_log("TLS disabled: no PSK\n");return false;}
    size_t n=fread(wifi_tls_psk,1,sizeof(wifi_tls_psk),f);int extra=fgetc(f);bool io=ferror(f);fclose(f);
    if(n!=sizeof(wifi_tls_psk)||extra!=EOF||io){memset(wifi_tls_psk,0,sizeof(wifi_tls_psk));resident_log("TLS disabled: invalid PSK\n");return false;}
    mbedtls_ssl_config_init(&wifi_tls_config);mbedtls_entropy_init(&wifi_tls_entropy);mbedtls_ctr_drbg_init(&wifi_tls_rng);
    static const unsigned char label[]="za-wifi-tls-v1";
    psa_status_t status=psa_crypto_init();
    if(status!=PSA_SUCCESS){resident_log("TLS PSA initialization failed: %d\n",(int)status);return false;}
    int rc=mbedtls_ctr_drbg_seed(&wifi_tls_rng,mbedtls_entropy_func,&wifi_tls_entropy,label,sizeof(label)-1);
    if(!rc)rc=mbedtls_ssl_config_defaults(&wifi_tls_config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
    if(rc){resident_log("TLS configuration failed: %d\n",rc);return false;}
    mbedtls_ssl_conf_rng(&wifi_tls_config,mbedtls_ctr_drbg_random,&wifi_tls_rng);
    mbedtls_ssl_conf_min_tls_version(&wifi_tls_config,MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&wifi_tls_config,MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_ciphersuites(&wifi_tls_config,wifi_tls_ciphers);
    mbedtls_ssl_conf_tls13_key_exchange_modes(&wifi_tls_config,MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_SRV_C)
    mbedtls_ssl_conf_new_session_tickets(&wifi_tls_config,0);
#endif
#ifdef MBEDTLS_SSL_EARLY_DATA
    mbedtls_ssl_conf_early_data(&wifi_tls_config,MBEDTLS_SSL_EARLY_DATA_DISABLED);
#endif
    rc=mbedtls_ssl_conf_psk(&wifi_tls_config,wifi_tls_psk,sizeof(wifi_tls_psk),label,sizeof(label)-1);
    memset(wifi_tls_psk,0,sizeof(wifi_tls_psk));
    if(rc){resident_log("TLS PSK configuration failed: %d\n",rc);return false;}
    return true;
}
#endif
#ifdef ZA_ISOLATED_TEST
static int last_accept_error;
static u64 heartbeat_tick;
static unsigned heartbeat_count;

// Bounded wake diagnostics: enough to distinguish a stopped main loop from a
// live but invalid listener without writing indefinitely to the SD card.
static void wifi_diagnostic_heartbeat(void) {
    u64 now=svcGetSystemTick();
    if(heartbeat_count>=150 || (heartbeat_tick && armTicksToNs(now-heartbeat_tick)<2000000000ULL))return;
    heartbeat_tick=now;heartbeat_count++;
    struct sockaddr_in addr={0};socklen_t len=sizeof(addr);
    int name_rc=wifi_listener<0?-1:getsockname(wifi_listener,(struct sockaddr*)&addr,&len);
    int name_errno=name_rc<0?errno:0;
    int error=0;socklen_t error_len=sizeof(error);
    int opt_rc=wifi_listener<0?-1:getsockopt(wifi_listener,SOL_SOCKET,SO_ERROR,&error,&error_len);
    int opt_errno=opt_rc<0?errno:0;
    resident_log("wake-diag n=%u tick=%" PRIu64 " fd=%d name=%d/%d port=%u opt=%d/%d so_error=%d usb=%u\n",
        heartbeat_count,now,wifi_listener,name_rc,name_errno,ntohs(addr.sin_port),opt_rc,opt_errno,error,usb_ready);
}
#endif
static bool wifi_open_listener(void) {
    wifi_listener=socket(AF_INET,SOCK_STREAM,0);
    if(wifi_listener<0){resident_log("Wi-Fi socket creation failed: %d\n",errno);return false;}
    int yes=1;
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_ANY);
#ifdef ZA_TLS_TEST
    addr.sin_port=htons(6013);
#else
    addr.sin_port=htons(6012);
#endif
    if(setsockopt(wifi_listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)) ||
       bind(wifi_listener,(struct sockaddr*)&addr,sizeof(addr)) ||
       listen(wifi_listener,8) || fcntl(wifi_listener,F_SETFL,O_NONBLOCK)) {
        resident_log("Wi-Fi bind/listen failed: %d\n",errno);close(wifi_listener);wifi_listener=-1;return false;
    }
#ifdef ZA_TLS_TEST
    resident_log("TLS test listener on 6013\n");
#elif defined(ZA_WIFI_TLS)
    resident_log("TLS Wi-Fi control listening on 6012\n");
#else
    resident_log("Authenticated Wi-Fi control listening on 6012\n");
#endif
    return true;
}
static void wifi_start(void) {
    resident_log("Wi-Fi: opening key\n");
    FILE *f=fopen("sdmc:/config/za-usb-control/wifi.key","rb");
    if(!f){resident_log("Wi-Fi disabled: no key\n");return;}
    size_t n=fread(wifi_key,1,sizeof(wifi_key),f);int extra=fgetc(f);fclose(f);
    if(n!=sizeof(wifi_key)||extra!=EOF){memset(wifi_key,0,sizeof(wifi_key));resident_log("Wi-Fi disabled: invalid key\n");return;}
#ifdef ZA_WIFI_TLS
    if(!wifi_tls_init())return;
#endif
    resident_log("Wi-Fi: initializing sockets\n");
    Result rc=socketInitializeDefault();
    resident_log("Wi-Fi: socket init returned %08X\n",rc);
    if(R_FAILED(rc)){resident_log("Wi-Fi socket initialization failed %08X\n",rc);return;}
    rc=nifmInitialize(NifmServiceType_Admin);wifi_nifm_ready=R_SUCCEEDED(rc);
    if(!wifi_nifm_ready)resident_log("Wi-Fi network status unavailable %08X\n",rc);
    if(!wifi_open_listener())wifiRecoveryFault(&wifi_recovery);
}
static void wifi_monitor(void) {
    u64 now=svcGetSystemTick();
    if(wifi_health_tick && armTicksToNs(now-wifi_health_tick)<1000000000ULL)return;
    wifi_health_tick=now;if(!wifi_nifm_ready)return;
    u32 ip=0;if(R_FAILED(nifmGetCurrentIpAddress(&ip)))ip=0;
    if(ip!=wifi_current_ip){native_network_transition();wifi_current_ip=ip;if(ip)native_network_resume();}
}

#ifdef CONTROL_NATIVE_HTTP
#include "api-native.h"
#endif

static void native_network_transition(void) {
#ifdef ZA_WIFI_TLS
    if(disc_socket>=0)close(disc_socket);
    disc_socket=-1;disc_ip=0;disc_attempted=false;disc_tick=0;
    if(input_job.active)input_stop("network-changed");
    if(sd_task.active)sd_task_end("network-cancelled");
    sd_abort();
#endif
    wifi_close_peers();if(wifi_listener>=0)close(wifi_listener);wifi_listener=-1;
#ifdef CONTROL_NATIVE_HTTP
    if(api_listener>=0){close(api_listener);}api_listener=-1;
#endif
    wifi_current_ip=UINT32_MAX;
}
static void native_network_resume(void) {
    (void)wifi_open_listener();
#ifdef CONTROL_NATIVE_HTTP
    (void)api_open_listener();
#endif
}
#ifdef ZA_WIFI_TLS
#define NATIVE_CONFIG_SLEEP() native_dns_cancel_sleep()
#include "power-lifecycle.h"
#endif

// Each unauthenticated peer has an absolute deadline and is polled without
// blocking. A stalled or fragmented challenge cannot stop USB or other peers.
#define WIFI_MAX_PEERS 8
#define WIFI_COMMAND_MAX 2048
typedef struct {
    int fd;
    bool used;
#ifdef ZA_WIFI_TLS
    mbedtls_ssl_context tls;
    bool tls_established;
#ifdef CONTROL_NATIVE_HTTP
    bool http;
    ApiRequest *request;
#endif
#endif
    u64 started;
    u8 nonce[32],input[2+WIFI_COMMAND_MAX+32];
    size_t nonce_sent,received,wanted;
} WifiPeer;
static WifiPeer wifi_peers[WIFI_MAX_PEERS];
#ifdef ZA_WIFI_TLS
static int wifi_tls_send(void *ctx,const unsigned char *buf,size_t len) {
    WifiPeer *p=ctx;ssize_t n=send(p->fd,buf,len,0);
    if(n<0)return (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)?MBEDTLS_ERR_SSL_WANT_WRITE:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}
static int wifi_tls_recv(void *ctx,unsigned char *buf,size_t len) {
    WifiPeer *p=ctx;ssize_t n=recv(p->fd,buf,len,0);
    if(n<0)return (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)?MBEDTLS_ERR_SSL_WANT_READ:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}
static bool wifi_tls_write_all(int fd,const char *data,size_t size) {
#ifdef CONTROL_NATIVE_HTTP
    if(api_capture_fd==fd)return api_append(data,size);
#endif
    WifiPeer *p=NULL;
    for(size_t i=0;i<WIFI_MAX_PEERS;i++)if(wifi_peers[i].used&&wifi_peers[i].fd==fd){p=&wifi_peers[i];break;}
    if(!p||!p->tls_established)return false;
    u64 started=svcGetSystemTick();
    while(size) {
        size_t chunk=size<MBEDTLS_SSL_OUT_CONTENT_LEN?size:MBEDTLS_SSL_OUT_CONTENT_LEN;
        int rc=mbedtls_ssl_write(&p->tls,(const unsigned char *)data,chunk);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(armTicksToNs(svcGetSystemTick()-started)>=2000000000ULL)return false;
            svcSleepThread(5000000ULL);continue;
        }
        if(rc<=0)return false;
        data+=rc;size-=(size_t)rc;started=svcGetSystemTick();
    }
    return true;
}
static bool wifi_tls_read_all(int fd,void *data,size_t size) {
#ifdef CONTROL_NATIVE_HTTP
    if(api_capture_fd==fd)return api_read(data,size);
#endif
    WifiPeer *p=NULL;
    for(size_t i=0;i<WIFI_MAX_PEERS;i++)if(wifi_peers[i].used&&wifi_peers[i].fd==fd){p=&wifi_peers[i];break;}
    if(!p||!p->tls_established)return false;
    unsigned char *out=data;u64 started=svcGetSystemTick();
    while(size) {
        int rc=mbedtls_ssl_read(&p->tls,out,size);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE){
            if(armTicksToNs(svcGetSystemTick()-started)>=2000000000ULL)return false;
            svcSleepThread(5000000ULL);continue;
        }
        if(rc<=0)return false;
        out+=rc;size-=(size_t)rc;
        if(armTicksToNs(svcGetSystemTick()-started)>=2000000000ULL)return false;
    }
    return true;
}
static bool wifi_tls_close_notify(WifiPeer *p) {
    u64 started=svcGetSystemTick();
    for(;;) {
        int rc=mbedtls_ssl_close_notify(&p->tls);
        if(!rc)return true;
        if(rc!=MBEDTLS_ERR_SSL_WANT_READ&&rc!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;
        if(armTicksToNs(svcGetSystemTick()-started)>=2000000000ULL)return false;
        svcSleepThread(5000000ULL);
    }
}
#endif
static void wifi_close_peer(WifiPeer *p) {
    if(p->used) {
#ifdef ZA_WIFI_TLS
        mbedtls_ssl_free(&p->tls);
#ifdef CONTROL_NATIVE_HTTP
        free(p->request);
#endif
#endif
        close(p->fd);
    }
    memset(p,0,sizeof(*p));
}
static void wifi_close_peers(void) {
    for(size_t i=0;i<WIFI_MAX_PEERS;i++)wifi_close_peer(&wifi_peers[i]);
}
#ifndef ZA_WIFI_TLS
static bool wifi_would_block(void) {
    return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR;
}
#endif
#ifdef CONTROL_NATIVE_HTTP
static void api_reply_peer(WifiPeer *p,int status){
    const char *type="application/json";const unsigned char *body=api_output;size_t size=api_output_size;
    if(status==200&&!strcmp(p->request->target,"/certificate")){type="application/x-pem-file";body=api_cert_pem;size=strlen((char*)api_cert_pem);}
    else if(status==200&&!strcmp(p->request->method,"GET")&&(!strncmp(p->request->target,"/screenshot",11)||!strncmp(p->request->target,"/files?",7)))type=!strncmp(p->request->target,"/screenshot",11)?"image/jpeg":"application/octet-stream";
    if(status>=400&&!size){body=(const unsigned char*)"{\"error\":\"request rejected\"}";size=strlen((char*)body);}
    if(status==202)size=0;
    char header[600];int n=snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: %s\r\nKeep-Alive: timeout=5\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n%s\r\n",status,status==200?"OK":status==202?"Accepted":"Error",type,(unsigned long)size,p->request->close||status>=400?"close":"keep-alive",status==401?"WWW-Authenticate: Bearer realm=\"Switch Control\"\r\n":status==405?"Allow: POST\r\n":"");
    bool sent=n>0&&(size_t)n<sizeof(header)&&wifi_tls_write_all(p->fd,header,(size_t)n)&&(!size||wifi_tls_write_all(p->fd,(const char*)body,size));
    if(!sent||p->request->close||status>=400){if(sent)(void)wifi_tls_close_notify(p);wifi_close_peer(p);return;}
    memset(p->request,0,sizeof(*p->request));p->started=svcGetSystemTick();
}
static void api_peer_poll(WifiPeer *p){
    ApiRequest *r=p->request;size_t wanted=r->header?r->header+r->body:8192;
    int n=mbedtls_ssl_read(&p->tls,r->data+r->used,wanted-r->used);
    if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE)return;
    if(n<=0){wifi_close_peer(p);return;}r->used+=(size_t)n;api_output_size=0;
    if(!r->header){int result=api_headers(r,wifi_key);if(!result)return;if(result!=200){api_reply_peer(p,result);return;}}
    if(r->used<r->header+r->body)return;
    int status=(!strcmp(r->method,"GET")&&!strcmp(r->target,"/certificate"))?200:api_rest(p->fd,r);
    if(status==200&&api_output_size>=3&&!memcmp(api_output,"ERR",3))status=409;
    if(status==200&&strcmp(r->target,"/mcp")&&api_output_size&&api_output[0]=='{'){ApiJson j;if(aj_parse(&j,(char*)api_output,api_output_size)&&aj_get(&j,0,"error")>=0)status=409;}
    api_reply_peer(p,status);
}
static void api_accept_peers(void){
    if(api_listener<0)return;
    unsigned active=0;for(size_t i=0;i<WIFI_MAX_PEERS;i++)if(wifi_peers[i].used&&wifi_peers[i].http)active++;
    if(active>=4)return;
    int fd=accept(api_listener,NULL,NULL);if(fd<0)return;size_t i=0;while(i<WIFI_MAX_PEERS&&wifi_peers[i].used)i++;
    if(i==WIFI_MAX_PEERS||fcntl(fd,F_SETFL,O_NONBLOCK)){close(fd);return;}WifiPeer *p=&wifi_peers[i];memset(p,0,sizeof(*p));p->request=calloc(1,sizeof(ApiRequest));if(!p->request){close(fd);return;}p->used=true;p->http=true;p->fd=fd;p->started=svcGetSystemTick();mbedtls_ssl_init(&p->tls);
    int no_delay=1;(void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&no_delay,sizeof(no_delay));
    if(mbedtls_ssl_setup(&p->tls,&api_tls_config)){wifi_close_peer(p);return;}mbedtls_ssl_set_bio(&p->tls,p,wifi_tls_send,wifi_tls_recv,NULL);
}
#endif
static void wifi_poll_peer(WifiPeer *p,u64 now) {
#ifdef ZA_WIFI_TLS
    if(armTicksToNs(now-p->started)>=5000000000ULL){wifi_close_peer(p);return;}
    if(!p->tls_established) {
        int rc=mbedtls_ssl_handshake(&p->tls);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return;
        if(rc){wifi_close_peer(p);return;}
        p->tls_established=true;
    }
#ifdef CONTROL_NATIVE_HTTP
    if(p->http){api_peer_poll(p);return;}
#endif
#else
    if(armTicksToNs(now-p->started)>=2000000000ULL){wifi_close_peer(p);return;}
#endif
    if(p->nonce_sent<sizeof(p->nonce)) {
#ifdef ZA_WIFI_TLS
        ssize_t n=mbedtls_ssl_write(&p->tls,p->nonce+p->nonce_sent,sizeof(p->nonce)-p->nonce_sent);
        if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE)return;
        if(n<0){wifi_close_peer(p);return;}
#else
        ssize_t n=send(p->fd,p->nonce+p->nonce_sent,sizeof(p->nonce)-p->nonce_sent,0);
        if(n<0){if(!wifi_would_block())wifi_close_peer(p);return;}
#endif
        if(!n){wifi_close_peer(p);return;}
        p->nonce_sent+=(size_t)n;
        if(p->nonce_sent<sizeof(p->nonce))return;
    }
    ssize_t n;
#ifdef ZA_WIFI_TLS
    n=mbedtls_ssl_read(&p->tls,p->input+p->received,p->wanted-p->received);
    if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE)return;
    if(n<0){wifi_close_peer(p);return;}
#else
    n=recv(p->fd,p->input+p->received,p->wanted-p->received,0);
    if(n<0){if(!wifi_would_block())wifi_close_peer(p);return;}
#endif
    if(!n){wifi_close_peer(p);return;}
    p->received+=(size_t)n;
    if(p->received<p->wanted)return;
    if(p->wanted==2) {
        size_t len=((size_t)p->input[0]<<8)|p->input[1];
        if(!len||len>WIFI_COMMAND_MAX){wifi_close_peer(p);return;}
        p->wanted=2+len+32;return;
    }
    size_t len=p->wanted-2-32;
    const u8 *message=p->input+2,*given=message+len;
    if(memchr(message,0,len)||memchr(message,'\n',len)||memchr(message,'\r',len)) {wifi_close_peer(p);return;}
    u8 packet[32+WIFI_COMMAND_MAX],expected[32];char line[WIFI_COMMAND_MAX+1];
    memcpy(packet,p->nonce,32);memcpy(packet+32,message,len);
    hmacSha256CalculateMac(expected,wifi_key,sizeof(wifi_key),packet,32+len);
    u8 diff=0;for(size_t i=0;i<32;i++)diff|=given[i]^expected[i];
    if(diff){wifi_close_peer(p);return;}
    memcpy(line,message,len);line[len]=0;
    // Authenticated commands remain serialized and have bounded socket I/O.
    struct timeval timeout={.tv_sec=2,.tv_usec=0};
    if(fcntl(p->fd,F_SETFL,0)||setsockopt(p->fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))||
       setsockopt(p->fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))) {wifi_close_peer(p);return;}
    wifiRecoveryAuthenticated(&wifi_recovery);
    bool sent;
    if(!strncmp(line,"press ",6)||!strncmp(line,"release ",8))sent=send_all(p->fd,"ERR use-click\n",14);
    else sent=dispatch(p->fd,line);
#ifdef ZA_WIFI_TLS
    if(sent)(void)wifi_tls_close_notify(p);
#else
    (void)sent;
#endif
    wifi_close_peer(p);
}
static void wifi_poll(void) {
    if(wifi_listener<0)return;
#ifdef CONTROL_NATIVE_HTTP
    api_accept_peers();
#endif
    for(size_t attempt=0;attempt<WIFI_MAX_PEERS;attempt++) {
        int fd=accept(wifi_listener,NULL,NULL);
        if(fd<0) {
#ifdef ZA_ISOLATED_TEST
            if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR && errno!=last_accept_error) {
                last_accept_error=errno;
                resident_log("Wi-Fi accept failed: errno=%d result=%08X\n",errno,socketGetLastResult());
            }
#endif
            break;
        }
#ifdef ZA_ISOLATED_TEST
        last_accept_error=0;
#endif
        size_t i=0;while(i<WIFI_MAX_PEERS&&wifi_peers[i].used)i++;
        if(i==WIFI_MAX_PEERS||fcntl(fd,F_SETFL,O_NONBLOCK)){close(fd);continue;}
        WifiPeer *p=&wifi_peers[i];memset(p,0,sizeof(*p));
        p->fd=fd;p->used=true;p->started=svcGetSystemTick();p->wanted=2;
#ifdef ZA_WIFI_TLS
        mbedtls_ssl_init(&p->tls);
        if(mbedtls_ssl_setup(&p->tls,&wifi_tls_config)){wifi_close_peer(p);continue;}
        mbedtls_ssl_set_bio(&p->tls,p,wifi_tls_send,wifi_tls_recv,NULL);
#endif
        randomGet(p->nonce,sizeof(p->nonce));
    }
    u64 now=svcGetSystemTick();
    for(size_t i=0;i<WIFI_MAX_PEERS;i++)if(wifi_peers[i].used)wifi_poll_peer(&wifi_peers[i],now);
}
int main(int argc,char **argv) {
    (void)argc;(void)argv;reader_started=svcGetSystemTick();
    resident_log("USB control startup %08X HOS %u.%u.%u\n",startup_rc,
        HOSVER_MAJOR(hosversionGet()),HOSVER_MINOR(hosversionGet()),HOSVER_MICRO(hosversionGet()));
    if(R_FAILED(startup_rc))return 1;
#ifdef ZA_ISOLATED_TEST
    Result isolation_rc=isolated_service_guard();
    if(R_FAILED(isolation_rc)) {
        resident_log("Isolated test refused: resident peer or PM error %08X\n",isolation_rc);
        return 1;
    }
#endif
    const UsbCommsInterfaceInfo info={0xFF,0x5A,0x02};
#ifdef ZA_WIFI_TLS
    native_power_start();
#endif
    wifi_start();
#ifdef CONTROL_NATIVE_HTTP
    api_start();
#endif
#ifdef ZA_TLS_TEST
    // The installed service owns USB; the isolated TLS test must not disturb it.
    Result rc=0;usb_ready=false;(void)info;
#else
    Result rc=usbCommsInitializeEx(1,&info,0x057e,0x3000);
    usb_ready=R_SUCCEEDED(rc);
    if(!usb_ready)resident_log("USB init failed: %08X; Wi-Fi remains available if configured\n",rc);
#endif
    bool pending=false,overflow=false,connected=false;u32 urb=0;size_t used=0;char line[256];
    while(true) {
#ifdef ZA_WIFI_TLS
        native_power_poll();
        if(resident_suspended){svcSleepThread(5000000ULL);continue;}
        input_poll();sd_task_poll();native_dns_poll();
#endif
        wifi_monitor();
#ifdef ZA_ISOLATED_TEST
        wifi_diagnostic_heartbeat();
#endif
        wifi_poll();
#ifdef ZA_WIFI_TLS
        discovery_poll();
#endif
        if(controller_ready && state.buttons && !INPUT_RUNNING && armTicksToNs(svcGetSystemTick()-last_control)>3000000000ULL)neutral();
        if(!usb_ready){svcSleepThread(5000000ULL);continue;}
        u32 link=0;rc=usbDsGetState(&link);if(R_FAILED(rc)){(void)detach_controller();usbCommsExit();usb_ready=false;continue;}
        if(link==5)connected=true;
        if(connected && link!=5){
            // A physical link transition cancels in-flight endpoints. Rebuild
            // once for that transition; do not replay a partial command.
            Result cleanup=detach_controller();
            resident_log("USB detach: controller cleanup %08X\n",cleanup);
            usbCommsExit();pending=false;connected=false;used=0;overflow=false;
            rc=usbCommsInitializeEx(1,&info,0x057e,0x3000);
            resident_log("USB detach: neutralized and rebuilt endpoints %08X\n",rc);
            if(R_FAILED(rc))usb_ready=false;
            continue;
        }
        if(controller_ready && state.buttons && !INPUT_RUNNING && armTicksToNs(svcGetSystemTick()-last_control)>3000000000ULL)neutral();
        if(!pending && link==5) {rc=usbCommsReadAsync(usb_in,sizeof(usb_in),&urb,0);if(R_FAILED(rc)){(void)detach_controller();usbCommsExit();usb_ready=false;continue;}pending=true;}
        if(pending && R_SUCCEEDED(eventWait(usbCommsGetReadCompletionEvent(0),0))) {
            u32 n=0,length=0;rc=usbCommsGetReadResult(urb,&n,0);pending=false;memcpy(&length,usb_in,4);
            if(R_FAILED(rc) || n!=sizeof(usb_in) || !length || length>sizeof(usb_in)-4){(void)detach_controller();usbCommsExit();usb_ready=false;continue;}
            for(u32 i=0;usb_ready && i<length;++i) {
                char ch=(char)usb_in[4+i];if(ch=='\r')continue;
                if(ch=='\n'){line[used]=0;bool ok=overflow?send_all(-1,"ERR command-too-long\n",21):dispatch(-1,line);
                    used=0;overflow=false;if(!ok){(void)detach_controller();usbCommsExit();usb_ready=false;}
                }else if(!overflow && used<sizeof(line)-1)line[used++]=ch;else overflow=true;
            }
        }
        svcSleepThread(5000000ULL);
    }
    return 0;
}
