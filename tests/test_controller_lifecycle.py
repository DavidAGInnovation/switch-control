"""Exercise the actual resident controller lifecycle with injected API failures."""
import pathlib
import subprocess
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[1] / 'firmware/usb-resident/source/main.c'

PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint32_t Result;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { int x, y; } HidAnalogStickState;
typedef struct { int deviceType, npadInterfaceType, singleColorBody, singleColorButtons, colorLeftGrip, colorRightGrip; } HiddbgHdlsDeviceInfo;
typedef struct { uint64_t buttons; int battery_level; HidAnalogStickState analog_stick_l, analog_stick_r; } HiddbgHdlsState;
#define R_FAILED(x) ((x) != 0)
#define R_SUCCEEDED(x) ((x) == 0)
#define Module_Libnx 1
#define LibnxError_BadInput 99
#define LibnxError_NotInitialized 98
#define MAKERESULT(a,b) ((a)+(b))
#define HidDeviceType_FullKey3 1
#define HidNpadInterfaceType_Bluetooth 1
#define RGBA8_MAXALPHA(a,b,c) 0
static int session, controller;
static HiddbgHdlsState state;
static bool controller_ready, hiddbg_ready, controller_owned_by_wifi;
static Result controller_cleanup_error;
static u32 controller_stage;
static Result controller_stage_result;
static unsigned char controller_work[4096];
static uint64_t last_control;
static Result detach_controller(void);
static int init_rc, work_rc, attach_rc, set_rc, detach_rc, release_rc;
static int is_rc;
static bool is_attached = true;
static int inits, attaches, sets, detaches, releases, exits;
static Result peer_result = 0x20f;
static Result pmdmntInitialize(void){return 0;}
static Result pmdmntGetProcessId(u64 *p,u64 id){(void)p;(void)id;return peer_result;}
static void pmdmntExit(void){}
static Result hiddbgInitialize(void){++inits;return init_rc;}
static Result hiddbgAttachHdlsWorkBuffer(int *s,void *b,unsigned n){(void)s;(void)b;(void)n;return work_rc;}
static Result hiddbgAttachHdlsVirtualDevice(int *c,const HiddbgHdlsDeviceInfo *i){(void)c;(void)i;++attaches;return attach_rc;}
static Result hiddbgIsHdlsVirtualDeviceAttached(int s,int c,bool *out){(void)s;(void)c;*out=is_attached;return is_rc;}
static Result hiddbgSetHdlsState(int c,const HiddbgHdlsState *s){(void)c;(void)s;++sets;return set_rc;}
static Result hiddbgDetachHdlsVirtualDevice(int c){(void)c;++detaches;return detach_rc;}
static Result hiddbgReleaseHdlsWorkBuffer(int s){(void)s;++releases;return release_rc;}
static void hiddbgExit(void){++exits;}
static uint64_t svcGetSystemTick(void){return 1;}
'''

CASES = r'''
static void reset(void){controller_ready=hiddbg_ready=false;controller_cleanup_error=0;controller_stage=controller_stage_result=0;peer_result=0x20f;init_rc=work_rc=attach_rc=set_rc=detach_rc=release_rc=is_rc=0;is_attached=true;inits=attaches=sets=detaches=releases=exits=0;memset(&state,0,sizeof(state));}
int main(void){
 reset();assert(ensure_controller(-1)==0);state.buttons=4;assert(detach_controller()==0);assert(!controller_ready&&!hiddbg_ready&&state.buttons==0);assert(detaches==1&&releases==1&&exits==1);assert(ensure_controller(-1)==0&&attaches==2);
 reset();assert(ensure_controller(-1)==0);detach_rc=42;assert(detach_controller()==42);assert(controller_ready&&hiddbg_ready&&releases==0&&exits==0);detach_rc=0;assert(ensure_controller(-1)==42&&attaches==1);
 reset();assert(ensure_controller(-1)==0);release_rc=43;assert(detach_controller()==43);assert(!controller_ready&&hiddbg_ready&&exits==0);release_rc=0;assert(ensure_controller(-1)==43&&attaches==1);
 reset();attach_rc=44;release_rc=45;assert(ensure_controller(-1)==45);assert(hiddbg_ready&&exits==0);attach_rc=release_rc=0;assert(ensure_controller(-1)==45&&attaches==1);
 reset();assert(ensure_controller(-1)==0);set_rc=46;assert(detach_controller()==46);assert(!controller_ready&&!hiddbg_ready&&detaches==1&&releases==1);
 reset();peer_result=0;assert(ensure_controller(-1)==100&&attaches==0);peer_result=0x20f;assert(ensure_controller(-1)==0);peer_result=0;assert(ensure_controller(-1)==100&&!controller_ready&&detaches==1);
 reset();work_rc=55;assert(ensure_controller(-1)==55&&controller_stage==3&&controller_stage_result==55&&attaches==0);
 reset();attach_rc=56;assert(ensure_controller(-1)==56&&controller_stage==4&&controller_stage_result==56&&releases==1);
 reset();set_rc=57;assert(ensure_controller(-1)==57&&controller_stage==5&&controller_stage_result==57&&detaches==1&&releases==1);
 reset();assert(ensure_controller(-1)==0);is_attached=false;assert(ensure_controller(-1)==99&&!controller_ready&&!hiddbg_ready&&detaches==0&&releases==1);is_attached=true;assert(ensure_controller(-1)==0&&attaches==2);
 reset();assert(ensure_controller(-1)==0);is_attached=false;assert(detach_controller()==0&&!controller_ready&&detaches==0&&releases==1);
 reset();assert(ensure_controller(-1)==0);is_rc=58;assert(detach_controller()==58&&controller_ready&&controller_cleanup_error==58&&detaches==0&&releases==0);is_rc=0;assert(ensure_controller(-1)==58&&attaches==1);
 reset();is_attached=false;assert(ensure_controller(-1)==99&&!controller_ready&&!hiddbg_ready&&detaches==0&&releases==1);
 reset();is_rc=59;assert(ensure_controller(-1)==59&&controller_ready&&controller_cleanup_error==59&&detaches==0&&releases==0);
 reset();assert(ensure_controller(5)==0&&controller_owned_by_wifi);assert(ensure_controller(-1)==100);assert(finish_controller_input(5,0)==0&&!controller_ready&&detaches==1);
 reset();assert(ensure_controller(-1)==0&&!controller_owned_by_wifi);assert(ensure_controller(5)==100);assert(finish_controller_input(-1,0)==0&&controller_ready&&detaches==0);assert(finish_controller_input(-1,46)==46&&!controller_ready);
 reset();assert(ensure_controller(5)==0);detach_rc=42;assert(finish_controller_input(5,0)==42&&controller_ready&&controller_cleanup_error==42);
 reset();assert(ensure_controller(5)==0);detach_rc=42;assert(finish_controller_input(5,46)==46&&controller_cleanup_error==42);
 return 0;
}
'''


class ControllerLifecycleTest(unittest.TestCase):
    def test_native_lifecycle_with_failures(self):
        source = SOURCE.read_text()
        guard_start = source.index('static Result release_missing_controller(void) {')
        guard_end = source.index('void __libnx_initheap(', guard_start)
        start = source.index('static Result ensure_controller(int fd) {')
        end = source.index('static bool button(', start)
        with tempfile.TemporaryDirectory() as tmp:
            cfile = pathlib.Path(tmp) / 'test.c'
            exe = pathlib.Path(tmp) / 'test'
            cfile.write_text(PRELUDE + source[guard_start:guard_end] + source[start:end] + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
