from pathlib import Path
import hashlib,json,subprocess,tempfile,unittest,shlex
from tests.test_sd_files import DRIVER,HERE,path,sha
class FileJobsTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.build=tempfile.TemporaryDirectory();p=Path(cls.build.name);(p/'driver.c').write_text('#define SD_SYNC_LIMIT 65536\n'+DRIVER);cls.exe=p/'driver';flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True));subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-I'+str(HERE/'firmware/usb-resident/source'),str(p/'driver.c'),*flags,'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.build.cleanup()
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);(self.root/'switch').mkdir();self.p=subprocess.Popen([str(self.exe)],cwd=self.root,stdin=subprocess.PIPE,stdout=subprocess.PIPE)
 def tearDown(self):self.p.stdin.close();self.p.wait(5);self.p.stdout.close();self.tmp.cleanup()
 def cmd(self,text,body=b''):
  self.p.stdin.write(text.encode()+b'\n'+body);self.p.stdin.flush();return json.loads(self.p.stdout.readline())
 def finish(self,r):
  token=r['job']
  for _ in range(20):
   self.cmd('TEST_TICK');r=self.cmd('fsTaskInfo '+token)
   if not r['active']:return r
  self.fail('Job failed to progress')
 def upload(self,data,old='missing'):
  token=self.cmd(f'fsBegin {path("/switch/a")} {len(data)} {sha(data)} {old}')['upload']
  for at in range(0,len(data),65536):self.cmd(f'fsChunk {token} {at} {len(data[at:at+65536])}',data[at:at+65536])
  return token,self.cmd('fsCommit '+token)
 def test_verified_commit_progress_and_cancellation(self):
  data=b'abc'*30000;token,r=self.upload(data);self.assertTrue(r['active']);self.assertFalse((self.root/'switch/a').exists())
  self.assertEqual(self.cmd('fsUploadInfo '+token)['bytes'],len(data))
  done=self.finish(r);self.assertEqual(done['state'],'completed');self.assertEqual(done['sha256'],sha(data));self.assertEqual((self.root/'switch/a').read_bytes(),data)
  token,r=self.upload(b'new'*30000,sha(data));self.cmd('fsTaskCancel '+r['job']);self.assertEqual((self.root/'switch/a').read_bytes(),data);self.assertEqual(list((self.root/'switch').iterdir()),[self.root/'switch/a'])
 def test_published_replacement_cancel_restores_original(self):
  old=b'old'*30000;(self.root/'switch/a').write_bytes(old);token,r=self.upload(b'new'*30000,sha(old))
  for _ in range(4):self.cmd('TEST_TICK')
  info=self.cmd('fsTaskInfo '+r['job']);self.assertEqual(info['phase'],3)
  self.cmd('fsTaskCancel '+r['job']);self.assertEqual((self.root/'switch/a').read_bytes(),old)
 def test_hash_larger_than_previous_limit_is_incremental_and_cancellable(self):
  f=self.root/'switch/large'
  with f.open('wb') as out:out.truncate(65*1024*1024)
  info=self.cmd('fsInfo');self.assertEqual(info['max_file_bytes'],8*1024**3)
  r=self.cmd('fsHash '+path('/switch/large'));self.assertTrue(r['active']);self.assertEqual(r['bytes'],0)
  self.cmd('TEST_TICK');info=self.cmd('fsTaskInfo '+r['job']);self.assertEqual(info['bytes'],65536)
  self.assertEqual(self.cmd('fsTaskCancel '+r['job'])['state'],'cancelled');self.assertEqual(f.stat().st_size,65*1024*1024)
 def test_changed_destination_publish_failure_and_bad_hash(self):
  old=b'old'*30000;(self.root/'switch/a').write_bytes(old);token,r=self.upload(b'new'*30000,sha(old));(self.root/'switch/a').write_bytes(b'changed');self.assertEqual(self.finish(r)['state'],'destination-changed');self.assertEqual((self.root/'switch/a').read_bytes(),b'changed')
  (self.root/'switch/a').unlink();token,r=self.upload(b'new'*30000);self.cmd('TEST_FAIL_PUBLISH');self.assertEqual(self.finish(r)['state'],'publish-failed-original-restored');self.assertFalse((self.root/'switch/a').exists())
 def test_tail_binary_bytes_and_short_log(self):
  (self.root/'switch/log').write_bytes(b'first\nlast\x00\xff')
  self.p.stdin.write(('fsTail '+path('/switch/log')+' 6\n').encode());self.p.stdin.flush();header=self.p.stdout.readline();self.assertTrue(header.startswith(b'FILE '));self.assertEqual(self.p.stdout.read(6),b'last\x00\xff')
