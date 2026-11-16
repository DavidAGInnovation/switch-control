from pathlib import Path
import subprocess,tempfile,unittest
HERE=Path(__file__).parent
SOURCE=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t u32;typedef uint32_t Result;typedef int PscPmModuleId;typedef int PscPmState;typedef struct{int event;}PscPmModule;
enum{PscPmModuleId_Fs=27,PscPmState_ReadySleep=2,PscPmState_ReadySleepCritical=3,PscPmState_ReadyShutdown=5};
#define R_FAILED(rc) ((rc)!=0)
#define R_SUCCEEDED(rc) ((rc)==0)
static bool power_ready,resident_suspended,controller_ready=true,upload_open=true;
static struct{bool active;}sd_task={true};static unsigned acks,closes,stops;static int next,registration_error;
static Result pscmInitialize(void){return 0;}
static void pscmExit(void){}
static Result pscmGetPmModule(PscPmModule*m,PscPmModuleId id,const u32*d,size_t n,bool clear){(void)m;assert(id==0x5a43&&n==1&&d[0]==27&&clear);return registration_error;}
static Result eventWait(int*e,int timeout){(void)e;assert(timeout==0);return 0;}
static Result pscPmModuleGetRequest(PscPmModule*m,PscPmState*s,u32*f){(void)m;*s=next;*f=0;return 0;}
static Result pscPmModuleAcknowledge(PscPmModule*m,PscPmState s){(void)m;(void)s;if(next==2||next==3||next==5)assert(resident_suspended&&!controller_ready&&!upload_open&&!sd_task.active);acks++;return 0;}
static void input_stop(const char*s){(void)s;controller_ready=false;stops++;}
static void sd_task_end(const char*s){(void)s;sd_task.active=false;}
static void sd_abort(void){upload_open=false;}
static void native_network_transition(void){closes++;}
static bool sd_reply(int fd,const char*s){(void)fd;(void)s;return true;}
#include "power-lifecycle.h"
int main(void){
 registration_error=1;native_power_start();assert(!power_ready);native_power_poll();assert(!acks);
 registration_error=0;native_power_start();assert(power_ready);
 next=2;native_power_poll();assert(acks==1&&closes==1&&stops==1&&native_sleep_count==1);
 next=3;native_power_poll();assert(acks==2&&native_sleep_count==2);
 next=1;native_power_poll();assert(!resident_suspended&&native_wake_count==1&&closes==3);
 next=0;native_power_poll();assert(!resident_suspended&&native_wake_count==2&&acks==4);
 assert(native_lifecycle_status(1));return 0;
}
'''
class PowerLifecycleTest(unittest.TestCase):
 def test_actual_psc_cleanup_order_and_registration_failure(self):
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(SOURCE);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(HERE/'usb-resident/source'),str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
