from pathlib import Path
import json,subprocess,tempfile,unittest
HERE=Path(__file__).resolve().parents[1]
DRIVER=r'''
#define CLOCK_HOST_TEST 1
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef int32_t s32;typedef unsigned Result;
#define R_SUCCEEDED(x) ((x)==0)
#define R_FAILED(x) ((x)!=0)
#define MAKERESULT(a,b) 99
enum{TimeType_UserSystemClock,TimeType_NetworkSystemClock,TimeType_LocalSystemClock};
typedef struct{u16 year;u8 month,day,hour,minute,second,pad;}TimeCalendarTime;
typedef struct{int offset;}TimeCalendarAdditionalInfo;
typedef struct{char name[36];}TimeLocationName;
static unsigned handles,writes;static bool sync_on=true,saved_on=true,fail_time,fail_saved;static int candidates=1;
static u64 clock_value=1794848400,elapsed;
static Result timeInitialize(void){handles++;return 0;}static void timeExit(void){handles--;}
static Result setsysInitialize(void){handles++;return 0;}static void setsysExit(void){handles--;}
static Result clock_runtime_sync_get(bool *out){*out=sync_on;return 0;}static Result clock_runtime_sync_set(bool en){sync_on=en;return 0;}
static Result setsysIsUserSystemClockAutomaticCorrectionEnabled(bool *out){*out=saved_on;return 0;}
static Result setsysSetUserSystemClockAutomaticCorrectionEnabled(bool en){if(fail_saved){fail_saved=false;return 71;}saved_on=en;return 0;}
static Result timeGetCurrentTime(int type,u64 *out){(void)type;*out=clock_value;return 0;}
static Result timeSetCurrentTime(int type,u64 value){if(type!=TimeType_LocalSystemClock)abort();writes++;if(fail_time){fail_time=false;return 72;}clock_value=value;return 0;}
static Result timeToCalendarTimeWithMyRule(u64 ts,TimeCalendarTime*c,TimeCalendarAdditionalInfo*i){(void)ts;*c=(TimeCalendarTime){.year=2026,.month=11,.day=16,.hour=18,.minute=0};i->offset=3600;return 0;}
static Result timeGetDeviceLocationName(TimeLocationName*z){strcpy(z->name,"Europe/Madrid");return 0;}
static Result timeToPosixTimeWithMyRule(const TimeCalendarTime*c,u64*out,s32 max,s32*count){(void)c;(void)max;out[0]=1800000000;*count=candidates;return 0;}
static u64 svcGetSystemTick(void){return elapsed;}static u64 armTicksToNs(u64 x){return x;}
static bool sd_reply(int fd,const char*s){(void)fd;fputs(s,stdout);return true;}
static bool sd_string(int fd,const char*s){return sd_reply(fd,"\"")&&sd_reply(fd,s)&&sd_reply(fd,"\"");}
static bool sd_error(int fd,const char*s){(void)fd;printf("{\"error\":\"%s\"}\n",s);return true;}
#include "clock-controls.h"
int main(void){char line[512];setvbuf(stdout,NULL,_IONBF,0);while(fgets(line,sizeof(line),stdin)){
 line[strcspn(line,"\r\n")]=0;
 if(!strcmp(line,"FAIL_TIME")){fail_time=true;puts("{}");}
 else if(!strcmp(line,"FAIL_SAVED")){fail_saved=true;puts("{}");}
 else if(!strcmp(line,"AMBIGUOUS")){candidates=2;puts("{}");}
 else if(!strcmp(line,"GAP")){candidates=0;puts("{}");}
 else if(!strcmp(line,"COUNTS"))printf("{\"writes\":%u}\n",writes);
 else if(!strcmp(line,"clockInfo"))native_clock_info(1);
 else native_clock_set(1,line);
 if(handles)return 8;
}return 0;}
'''
class ClockControlsTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);(p/'driver.c').write_text(DRIVER);cls.exe=p/'driver';subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'firmware/usb-resident/source'),str(p/'driver.c'),'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def setUp(self):self.p=subprocess.Popen([str(self.exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
 def tearDown(self):self.p.stdin.close();self.p.wait(5);self.p.stdout.close()
 def cmd(self,s):self.p.stdin.write(s+'\n');self.p.stdin.flush();return json.loads(self.p.stdout.readline())
 def test_reads_and_updates_runtime_and_persistent_sync(self):
  r=self.cmd('clockInfo');self.assertEqual(r['timezone'],'Europe/Madrid');self.assertTrue(r['automatic_sync']);self.assertEqual(r['utc_offset_seconds'],3600)
  self.assertTrue(self.cmd('clockSync false')['ok']);r=self.cmd('clockInfo');self.assertFalse(r['automatic_sync']);self.assertFalse(r['saved_automatic_sync'])
  self.assertTrue(self.cmd('clockSync true')['ok']);self.assertTrue(self.cmd('clockInfo')['automatic_sync'])
 def test_manual_utc_and_local_disable_sync(self):
  self.assertTrue(self.cmd('clockSetUtc 1800000001')['ok']);r=self.cmd('clockInfo');self.assertEqual(r['timestamp'],1800000001);self.assertFalse(r['automatic_sync'])
  self.assertTrue(self.cmd('clockSetLocal 2028 2 29 23 59 59')['ok']);self.assertEqual(self.cmd('clockInfo')['timestamp'],1800000000)
 def test_failure_restores_time_and_sync_settings(self):
  before=self.cmd('clockInfo');self.cmd('FAIL_TIME');r=self.cmd('clockSetUtc 1800000000');self.assertEqual(r['result'],72);self.assertEqual(r['rollback_result'],0);after=self.cmd('clockInfo');self.assertEqual(before['timestamp'],after['timestamp']);self.assertTrue(after['automatic_sync']);self.assertTrue(after['saved_automatic_sync'])
  self.cmd('FAIL_SAVED');self.assertEqual(self.cmd('clockSync false')['result'],71);self.assertTrue(self.cmd('clockInfo')['automatic_sync'])
 def test_invalid_calendar_overflow_dst_and_utc_mixing(self):
  for command in ['clockSetLocal 2026 2 29 0 0 0','clockSetLocal 2026 4 31 0 0 0','clockSetLocal 2026 1 1 24 0 0','clockSetUtc 999999999999999999999999','clockSetUtc -1','clockSync maybe','clockSetLocal 4294969322 1 1 0 0 0']:
   self.assertIn('error',self.cmd(command),command)
  self.assertEqual(self.cmd('COUNTS')['writes'],0)
  self.cmd('AMBIGUOUS');self.assertEqual(self.cmd('clockSetLocal 2026 10 25 2 30 0')['error'],'ambiguous-date-use-utc')
  self.cmd('GAP');self.assertEqual(self.cmd('clockSetLocal 2026 3 29 2 30 0')['error'],'nonexistent-local-time');self.assertEqual(self.cmd('COUNTS')['writes'],0)
