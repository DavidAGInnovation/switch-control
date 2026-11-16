"""Exercise the actual C filesystem implementation with a temporary SD root."""
from pathlib import Path
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
import unittest

HERE=Path(__file__).parent
DRIVER=r'''
#define SD_HOST_TEST
#define SD_ROOT "."
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
static bool fail_publish,app_open;
static int test_rename(const char *a,const char *b){if(fail_publish&&strstr(a,".part")){fail_publish=false;errno=EIO;return -1;}return rename(a,b);}
#define rename test_rename
#include "sd-files.h"
#undef rename
static bool sd_send(int fd,const void *data,size_t n){(void)fd;return fwrite(data,1,n,stdout)==n;}
static bool sd_receive(int fd,void *data,size_t n){(void)fd;return fread(data,1,n,stdin)==n;}
static void sd_random(void *data,size_t n){static unsigned counter;memset(data,0x42,n);((unsigned char*)data)[0]=(unsigned char)++counter;}
static bool sd_mutation_allowed(const char *path){return !app_open||strncasecmp(path,"/atmosphere/",12);}
int main(void){setvbuf(stdout,NULL,_IONBF,0);char line[2049];while(fgets(line,sizeof(line),stdin)){line[strcspn(line,"\r\n")]=0;if(!strcmp(line,"TEST_TICK")){sd_task_poll();puts("{}");continue;}if(!strcmp(line,"TEST_FAIL_PUBLISH")){fail_publish=true;puts("{}");continue;}if(!strcmp(line,"TEST_APP_OPEN")){app_open=true;puts("{}");continue;}if(!strcmp(line,"TEST_APP_CLOSED")){app_open=false;puts("{}");continue;}if(!sd_files_command(1,line))return 3;}sd_abort();return 0;}
'''
def path(s):return s.encode().hex()
def sha(b):return hashlib.sha256(b).hexdigest()

class SdFilesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory();p=Path(cls.build.name);(p/'test.c').write_text(DRIVER);cls.exe=p/'test'
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-I'+str(HERE/'usb-resident/source'),str(p/'test.c'),*flags,'-o',str(cls.exe)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);(self.root/'switch').mkdir();(self.root/'atmosphere').mkdir();self.server=subprocess.Popen([str(self.exe)],cwd=self.root,stdin=subprocess.PIPE,stdout=subprocess.PIPE)
    def tearDown(self):
        self.server.stdin.close();self.server.wait(timeout=5);self.server.stdout.close();self.tmp.cleanup()
    def raw(self,text,body=b''):
        self.server.stdin.write(text.encode()+b'\n'+body);self.server.stdin.flush();line=self.server.stdout.readline();self.assertTrue(line,'Server stopped')
        if line.startswith(b'FILE '):return line,self.server.stdout.read(int(line.split()[2]))
        return json.loads(line)
    def begin(self,remote,data,old='missing'):return self.raw(f'fsBegin {path(remote)} {len(data)} {sha(data)} {old}')['upload']
    def chunk(self,token,data,offset=0):return self.raw(f'fsChunk {token} {offset} {len(data)}',data)
    def commit(self,token):return self.raw('fsCommit '+token)
    def upload(self,remote,data,old='missing'):
        t=self.begin(remote,data,old)
        for at in range(0,len(data),65536):self.assertTrue(self.chunk(t,data[at:at+65536],at)['ok'])
        return self.commit(t)
    def test_binary_multichunk_and_empty_roundtrip(self):
        for name,data in [('a.bin',bytes(range(256))*700),('empty',b'')]:
            remote='/switch/'+name;self.assertTrue(self.upload(remote,data)['ok']);self.assertEqual((self.root/remote[1:]).read_bytes(),data);self.assertEqual(self.raw('fsHash '+path(remote))['sha256'],sha(data))
            if data:
                header,body=self.raw(f'fsRead {path(remote)} 65000 65536');self.assertEqual(body,data[65000:130536])
    def test_long_unicode_and_spaces(self):
        name='/switch/'+'é a'*55+'.txt';self.assertGreater(len(path(name)),255);data=b'hello\x00\nworld';self.upload(name,data);self.assertEqual(self.raw('fsStat '+path(name))['size'],len(data));self.assertEqual(self.raw('fsList '+path('/switch')+' 0')['entries'][0]['name'],name.split('/')[-1])
    def test_replace_requires_exact_hash_and_preserves_destination_until_commit(self):
        target=self.root/'switch/a';target.write_bytes(b'old');self.assertIn('error',self.raw(f'fsBegin {path("/switch/a")} 3 {sha(b"new")} missing'))
        t=self.begin('/switch/a',b'new',sha(b'old'));self.chunk(t,b'new');self.assertEqual(target.read_bytes(),b'old');self.assertTrue(self.commit(t)['ok']);self.assertEqual(target.read_bytes(),b'new');self.assertEqual(len(list(target.parent.iterdir())),1)
    def test_changed_destination_and_bad_upload_hash_do_not_replace(self):
        target=self.root/'switch/a';target.write_bytes(b'old');t=self.begin('/switch/a',b'new',sha(b'old'));self.chunk(t,b'bad');self.assertIn('error',self.commit(t));self.assertEqual(target.read_bytes(),b'old')
        t=self.begin('/switch/a',b'new',sha(b'old'));self.chunk(t,b'new');target.write_bytes(b'other');self.assertIn('error',self.commit(t));self.assertEqual(target.read_bytes(),b'other')
    def test_publish_failure_restores_original(self):
        target=self.root/'switch/a';target.write_bytes(b'old');t=self.begin('/switch/a',b'new',sha(b'old'));self.chunk(t,b'new');self.raw('TEST_FAIL_PUBLISH');self.assertEqual(self.commit(t)['error'],'publish-failed-original-restored');self.assertEqual(target.read_bytes(),b'old');self.assertEqual(list(target.parent.iterdir()),[target])
    def test_partial_cancel_busy_offsets_and_range(self):
        t=self.begin('/switch/a',b'123456');self.chunk(t,b'12');self.assertEqual(self.commit(t)['error'],'incomplete-upload');self.assertEqual(self.raw(f'fsBegin {path("/switch/b")} 0 {sha(b"")} missing')['error'],'upload-busy');self.assertEqual(self.raw(f'fsChunk {t} 0 1')['error'],'chunk-range');self.assertEqual(self.raw(f'fsChunk {t} 2 65537')['error'],'chunk-range');self.assertEqual(self.raw('fsInfo')['upload'],t);self.assertTrue(self.raw('fsAbort '+t)['ok']);self.assertFalse(list((self.root/'switch').iterdir()))
    def test_path_traversal_credentials_service_and_symlinks_rejected(self):
        (self.root/'switch/link').symlink_to(self.root/'atmosphere',target_is_directory=True)
        for name in ['/switch/../x','/switch/./x','/switch//x','/switch/x/','/switch/x\n','/config/za-usb-control/tls.key','/CONFIG/ZA-USB-CONTROL/controller.key','/switch/link/x','/switch/.switch-control-abc.part','/sdmc:/x']:
            with self.subTest(name=name):self.assertEqual(self.raw('fsStat '+path(name))['error'],'path-denied')
        for name in ['/Nintendo/x','/atmosphere/contents/430000000000000D/exefs.nsp','/atmosphere/contents/430000000000000b/flags/boot2.flag']:
            self.assertEqual(self.raw('fsMkdir '+path(name))['error'],'path-denied')
    def test_open_application_blocks_cfw_change_both_at_start_and_commit(self):
        self.raw('TEST_APP_OPEN');self.assertEqual(self.raw('fsMkdir '+path('/atmosphere/test'))['error'],'path-denied');self.raw('TEST_APP_CLOSED');t=self.begin('/atmosphere/a',b'x');self.chunk(t,b'x');self.raw('TEST_APP_OPEN');self.assertEqual(self.commit(t)['error'],'destination-changed');self.assertFalse((self.root/'atmosphere/a').exists())
    def test_paging_move_delete_and_hash_guard(self):
        for i in range(70):(self.root/f'switch/{i}').write_bytes(b'x')
        a=self.raw('fsList '+path('/switch')+' 0');b=self.raw('fsList '+path('/switch')+' '+str(a['next']));self.assertEqual(len(a['entries'])+len(b['entries']),70);self.assertEqual(b['next'],-1)
        self.assertTrue(self.raw(f'fsMove {path("/switch/0")} {path("/switch/moved")} {sha(b"x")}')['ok']);self.assertIn('error',self.raw(f'fsMove {path("/switch/1")} {path("/switch/2")} {sha(b"x")}'));self.assertIn('error',self.raw(f'fsDelete {path("/switch/moved")} {sha(b"y")}'));self.assertTrue(self.raw(f'fsDelete {path("/switch/moved")} {sha(b"x")}')['ok'])
if __name__=='__main__':unittest.main()
