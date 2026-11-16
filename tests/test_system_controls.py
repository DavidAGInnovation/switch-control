from pathlib import Path
import json,subprocess,tempfile,unittest
HERE=Path(__file__).resolve().parents[1]
PRE=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
typedef unsigned Result;typedef uint32_t u32;typedef uint64_t u64;typedef unsigned PsmChargerType;typedef unsigned ColorSetId;
#define R_SUCCEEDED(x) ((x)==0)
#define R_FAILED(x) ((x)!=0)
#define MAKERESULT(a,b) 99
#define ColorSetId_Dark 1
#define ColorSetId_Light 0
typedef struct{char nickname[128];}SetSysDeviceNickName;
static unsigned handles;static float bright=.5f,vol=.3f;static bool automatic=true;static ColorSetId theme=1;static SetSysDeviceNickName nickname={"Test"};static bool controller_ready=false;static u64 reader_started;
static Result psmInitialize(void){handles++;return 0;}static void psmExit(void){handles--;}
static Result lblInitialize(void){handles++;return 0;}static void lblExit(void){handles--;}
static Result audctlInitialize(void){handles++;return 0;}static void audctlExit(void){handles--;}
static Result setsysInitialize(void){handles++;return 0;}static void setsysExit(void){handles--;}
static Result psmGetChargerType(PsmChargerType *out){*out=1;return 0;}static Result psmGetBatteryChargePercentage(u32 *out){*out=80;return 0;}
static Result lblIsAutoBrightnessControlEnabled(bool *out){*out=automatic;return 0;}static Result lblGetCurrentBrightnessSetting(float *out){*out=bright;return 0;}
static Result lblSetCurrentBrightnessSetting(float v){bright=v;return 0;}static Result lblApplyCurrentBrightnessSettingToBacklight(void){return 0;}static Result lblEnableAutoBrightnessControl(void){automatic=true;return 0;}static Result lblDisableAutoBrightnessControl(void){automatic=false;return 0;}
static Result audctlGetSystemOutputMasterVolume(float *out){*out=vol;return 0;}static Result audctlSetSystemOutputMasterVolume(float v){vol=v;return 0;}
static Result setsysGetDeviceNickname(SetSysDeviceNickName *out){*out=nickname;return 0;}static Result setsysSetDeviceNickname(const SetSysDeviceNickName *v){nickname=*v;return 0;}
static Result setsysGetColorSetId(ColorSetId *out){*out=theme;return 0;}static Result setsysSetColorSetId(ColorSetId v){theme=v;return 0;}
static Result nifmGetCurrentIpAddress(u32 *out){*out=htonl(INADDR_LOOPBACK);return 0;}
static u64 svcGetSystemTick(void){return 1000000000;}static u64 armTicksToNs(u64 v){return v;}static u32 hosversionGet(void){return 23;}
#define HOSVER_MAJOR(x) (x)
#define HOSVER_MINOR(x) 0
#define HOSVER_MICRO(x) 1
static bool sd_send(int fd,const void *p,size_t n){(void)fd;return fwrite(p,1,n,stdout)==n;}static bool sd_reply(int fd,const char *p){return sd_send(fd,p,strlen(p));}
static bool sd_string(int fd,const char *p){return sd_reply(fd,"\"")&&sd_reply(fd,p)&&sd_reply(fd,"\"");}static bool sd_error(int fd,const char *code){(void)code;return sd_reply(fd,"ERROR\n");}
static int sd_nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
#include "system-controls.h"
int main(int argc,char **argv){if(argc==1)system_status(1);else system_set(1,argv[1]);if(handles)return 8;return 0;}
'''
class SystemControlsTest(unittest.TestCase):
 def test_status_ranges_settings_and_service_lifetimes(self):
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(PRE);subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(HERE/'firmware/usb-resident/source'),str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
   status=json.loads(subprocess.check_output([str(p/'test')]));self.assertEqual(status['battery_percent'],80);self.assertEqual(status['volume'],.3);self.assertEqual(status['nickname'],'Test')
   for command in ['systemSet brightness .7','systemSet volume .2','systemSet theme light','systemSet auto_brightness false','systemSet nickname 48656c6c6f']:
    self.assertTrue(json.loads(subprocess.check_output([str(p/'test'),command]))['ok'])
   for command in ['systemSet volume nan','systemSet brightness 2','systemSet theme bad','systemSet nickname 00','systemSet auto_brightness maybe']:
    self.assertEqual(subprocess.check_output([str(p/'test'),command]),b'ERROR\n')
if __name__=='__main__':unittest.main()
