from pathlib import Path
import json,subprocess,tempfile,unittest
HERE=Path(__file__).parent
DRIVER=r'''
#include <assert.h>
#include "input-jobs.h"
static uint64_t now;static unsigned sets,releases;static bool fail_apply,fail_release;
static uint64_t input_now(void){return now;}
static bool input_apply(const InputStep *p){(void)p;sets++;return !fail_apply;}
static bool input_release(void){releases++;return !fail_release;}
static void input_random(void *p,size_t n){static unsigned char k;memset(p,++k,n);}
static bool input_reply(int fd,const char *s){(void)fd;fputs(s,stdout);return true;}
static bool input_error(int fd,const char *s){(void)fd;printf("{\"error\":\"%s\"}\n",s);return true;}
static bool input_available(void){return true;}
int main(void){char line[4096];setvbuf(stdout,NULL,_IONBF,0);while(fgets(line,sizeof(line),stdin)){
 line[strcspn(line,"\r\n")]=0;
 if(!strncmp(line,"TICK ",5)){now+=strtoull(line+5,NULL,10);input_poll();input_state_reply(1,false);}
 else if(!strcmp(line,"FAIL_APPLY")){fail_apply=true;puts("{}");}
 else if(!strcmp(line,"FAIL_RELEASE")){fail_release=true;puts("{}");}
 else if(!strcmp(line,"COUNTS"))printf("{\"sets\":%u,\"releases\":%u}\n",sets,releases);
 else input_command(1,line);
}return 0;}
'''
class InputJobsTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);(p/'driver.c').write_text(DRIVER);cls.exe=p/'driver';subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'usb-resident/source'),str(p/'driver.c'),'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def setUp(self):self.p=subprocess.Popen([str(self.exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
 def tearDown(self):self.p.stdin.close();self.p.wait(5);self.p.stdout.close()
 def command(self,s):self.p.stdin.write(s+'\n');self.p.stdin.flush();return json.loads(self.p.stdout.readline())
 def test_owner_expiry_and_no_resurrection(self):
  a=self.command('inputLease 1:100,0,0,0:500:0');token=a['action'];self.assertTrue(a['active'])
  self.assertIn('error',self.command('inputLease 2:0,0,0,0:500:0'))
  self.assertIn('error',self.command('inputRenew '+'a'*32+' 1000'))
  self.assertIn('error',self.command('inputRenew '+token+' 4294968296'))
  self.assertNotIn('action',self.command('inputInfo'))
  self.command('TICK 400');self.assertTrue(self.command('inputRenew '+token+' 1000')['active'])
  self.assertEqual(self.command('TICK 1000')['state'],'expired')
  self.assertFalse(self.command('inputRenew '+token+' 1000')['active'])
  released=self.command('COUNTS')['releases'];self.command('inputCancel '+token);self.assertEqual(self.command('COUNTS')['releases'],released)
  self.assertGreater(self.command('COUNTS')['releases'],0)
 def test_32_steps_nonblocking_cancellation_and_combined_axes(self):
  wire=';'.join(['41:32767,-32767,100,-100:10:1']*32)
  a=self.command('inputStart '+wire);self.assertEqual(a['steps'],32);self.assertEqual(self.command('COUNTS')['sets'],1)
  for _ in range(32):self.command('TICK 10');last=self.command('TICK 1')
  self.assertEqual(last['state'],'completed')
  a=self.command('inputStart 1:0,0,0,0:10000:0');self.assertEqual(self.command('inputCancel '+a['action'])['state'],'cancelled')
 def test_malformed_overflow_limits_and_cleanup_failure(self):
  for row in ['1:99999999999999999,0,0,0:1:0','1:0,0,0,0:4294967297:0','-1:0,0,0,0:1:0','1:0,0,0,0:10001:0','1:0,0,0,0:1:0;garbage',';'.join(['1:0,0,0,0:1:0']*33)]:
   self.assertIn('error',self.command('inputStart '+row),row)
  self.command('FAIL_RELEASE');a=self.command('inputLease 1:0,0,0,0:100:0');self.assertEqual(self.command('inputCancel '+a['action'])['state'],'cleanup-error')
 def test_apply_failure_does_not_leave_active_job(self):
  self.command('FAIL_APPLY');self.assertIn('error',self.command('inputStart 1:0,0,0,0:100:0'));self.assertFalse(self.command('inputInfo')['active'])
