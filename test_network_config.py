from pathlib import Path
import json,subprocess,tempfile,unittest
HERE=Path(__file__).parent
DRIVER=r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef unsigned Result;typedef uint64_t u64;typedef uint32_t u32;
typedef struct{unsigned char value[16];}Uuid;
typedef struct{unsigned char addr[4];}NifmIpV4Address;
typedef struct{unsigned char is_automatic;NifmIpV4Address primary_dns_server,secondary_dns_server;}NifmDnsSetting;
typedef struct{Uuid uuid;char password[32];struct{NifmDnsSetting dns_setting;char untouched[32];}ip_setting_data;}NifmNetworkProfileData;
#define R_SUCCEEDED(x) ((x)==0)
#define R_FAILED(x) ((x)!=0)
#define MAKERESULT(a,b) 99
static u64 now;static unsigned writes;static bool fail_write;static u32 wifi_current_ip=42;
static NifmNetworkProfileData saved={.uuid={{1}},.password="DO-NOT-EXPOSE",.ip_setting_data={.dns_setting={.is_automatic=0,.primary_dns_server={{1,1,1,1}},.secondary_dns_server={{9,9,9,9}}},.untouched="KEEP-ME"}};
static Result nifmGetCurrentNetworkProfile(NifmNetworkProfileData*p){*p=saved;return 0;}
static Result nifmGetNetworkProfile(Uuid id,NifmNetworkProfileData*p){if(memcmp(&id,&saved.uuid,sizeof(id)))return 12;*p=saved;return 0;}
static Result nifmSetNetworkProfile(const NifmNetworkProfileData*p,Uuid*out){writes++;if(fail_write)return 17;if(strcmp(p->password,"DO-NOT-EXPOSE")||strcmp(p->ip_setting_data.untouched,"KEEP-ME"))abort();saved=*p;*out=p->uuid;return 0;}
static u64 svcGetSystemTick(void){return now;}static u64 armTicksToNs(u64 v){return v;}
static void randomGet(void*p,size_t n){memset(p,0x42,n);}
static void sd_hex(char*out,const unsigned char*p,size_t n){for(size_t i=0;i<n;i++)sprintf(out+2*i,"%02x",p[i]);}
static bool sd_reply(int fd,const char*s){(void)fd;fputs(s,stdout);return true;}
static bool sd_error(int fd,const char*s){(void)fd;printf("{\"error\":\"%s\"}\n",s);return true;}
#include "network-config.h"
int main(void){char line[512];setvbuf(stdout,NULL,_IONBF,0);while(fgets(line,sizeof(line),stdin)){
 line[strcspn(line,"\r\n")]=0;
 if(!strcmp(line,"TICK")){now+=250000000ULL;native_dns_poll();native_dns_status(1);}
 else if(!strcmp(line,"FAIL")){fail_write=true;puts("{}");}
 else if(!strcmp(line,"CHANGE_PROFILE")){saved.uuid.value[0]++;puts("{}");}
 else if(!strcmp(line,"CHANGE_DNS")){saved.ip_setting_data.dns_setting.primary_dns_server.addr[0]++;puts("{}");}
 else if(!strcmp(line,"SLEEP")){native_dns_cancel_sleep();native_dns_status(1);}
 else if(!strcmp(line,"INFO"))printf("{\"writes\":%u,\"automatic\":%s,\"primary\":\"%u.%u.%u.%u\",\"listener_epoch\":%u}\n",writes,saved.ip_setting_data.dns_setting.is_automatic?"true":"false",saved.ip_setting_data.dns_setting.primary_dns_server.addr[0],saved.ip_setting_data.dns_setting.primary_dns_server.addr[1],saved.ip_setting_data.dns_setting.primary_dns_server.addr[2],saved.ip_setting_data.dns_setting.primary_dns_server.addr[3],wifi_current_ip);
 else if(!strcmp(line,"dnsStatus"))native_dns_status(1);
 else native_dns_schedule(1,line);
}return 0;}
'''
class NetworkConfigTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);(p/'driver.c').write_text(DRIVER);cls.exe=p/'driver';subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'usb-resident/source'),str(p/'driver.c'),'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def setUp(self):self.p=subprocess.Popen([str(self.exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
 def tearDown(self):self.p.stdin.close();self.p.wait(5);self.p.stdout.close()
 def cmd(self,s):
  self.p.stdin.write(s+'\n');self.p.stdin.flush();reply=self.p.stdout.readline();self.assertNotIn('DO-NOT-EXPOSE',reply);return json.loads(reply)
 def test_scheduled_manual_and_automatic_preserve_other_profile_fields(self):
  self.assertTrue(self.cmd('dnsSet false 8.8.8.8 0.0.0.0')['pending']);self.assertEqual(self.cmd('INFO')['writes'],0)
  self.assertEqual(self.cmd('TICK')['state'],'applied');r=self.cmd('INFO');self.assertEqual(r['primary'],'8.8.8.8');self.assertFalse(r['automatic']);self.assertEqual(r['listener_epoch'],4294967295)
  self.cmd('dnsSet true - -');self.assertEqual(self.cmd('TICK')['state'],'applied');r=self.cmd('INFO');self.assertTrue(r['automatic']);self.assertEqual(r['primary'],'8.8.8.8')
 def test_rejects_bad_ipv4_and_conflicting_requests_without_writes(self):
  for command in ['dnsSet false 0.0.0.0 1.1.1.1','dnsSet false 256.1.2.3 0.0.0.0','dnsSet false 224.0.0.1 0.0.0.0','dnsSet false 01.2.3.4 0.0.0.0','dnsSet false 1.1.1.1 trailing','dnsSet true 1.1.1.1 -','dnsSet yes - -']:
   self.assertIn('error',self.cmd(command),command)
  self.cmd('dnsSet true - -');self.assertIn('error',self.cmd('dnsSet true - -'));self.assertEqual(self.cmd('INFO')['writes'],0)
 def test_profile_or_dns_change_cancels_stale_request(self):
  self.cmd('dnsSet true - -');self.cmd('CHANGE_PROFILE');self.assertEqual(self.cmd('TICK')['state'],'profile-changed');self.assertEqual(self.cmd('INFO')['writes'],0)
  self.cmd('dnsSet true - -');self.cmd('CHANGE_DNS');self.assertEqual(self.cmd('TICK')['state'],'dns-settings-changed');self.assertEqual(self.cmd('INFO')['writes'],0)
 def test_failure_is_reported_once_and_sleep_cancels_pending_write(self):
  self.cmd('FAIL');self.cmd('dnsSet true - -');r=self.cmd('TICK');self.assertEqual(r['state'],'failed');self.assertEqual(r['result'],17);self.cmd('TICK');self.assertEqual(self.cmd('INFO')['writes'],1)
  self.cmd('dnsSet true - -');self.assertEqual(self.cmd('SLEEP')['state'],'sleep-cancelled');self.cmd('TICK');self.assertEqual(self.cmd('INFO')['writes'],1)
