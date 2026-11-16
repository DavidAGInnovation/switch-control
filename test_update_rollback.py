"""Execute the updater's actual restore ordering with filesystem fixtures."""
from pathlib import Path
import subprocess,tempfile,unittest
SOURCE=Path(__file__).parent/'file-update/source/main.c'
PRELUDE=r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
static const char *exe="current",*previous="previous";
static unsigned char old_hash[1]={'o'},new_hash[1]={'n'};
static bool stop_ok=true,start_ok=true,stopped,started;
static bool absent(const char *p){return access(p,F_OK)!=0;}
static bool hash_ok(const char *p,const unsigned char *hash){FILE *f=fopen(p,"rb");if(!f)return false;int c=fgetc(f);fclose(f);return c==*hash;}
static bool stop(void){stopped=true;return stop_ok;}
static bool start(void){started=true;return start_ok;}
static void write_file(const char *p,const char *s){FILE *f=fopen(p,"wb");assert(f);fputs(s,f);fclose(f);}
'''
CASES=r'''
int main(void){
 write_file(exe,"n");write_file(previous,"o");assert(restore());assert(stopped&&started&&hash_ok(exe,old_hash)&&absent(previous));
 stopped=started=false;write_file(exe,"n");write_file(previous,"x");assert(!restore());assert(!stopped&&!started&&hash_ok(exe,new_hash));
 write_file(previous,"o");stop_ok=false;assert(!restore());assert(!started&&hash_ok(exe,new_hash)&&hash_ok(previous,old_hash));
 stop_ok=true;write_file(exe,"x");assert(!restore());assert(hash_ok(previous,old_hash)&&!hash_ok(exe,new_hash));
 write_file(exe,"n");start_ok=false;assert(!restore());assert(hash_ok(exe,old_hash)&&absent(previous));
 return 0;
}
'''
class UpdateRollbackTest(unittest.TestCase):
 def test_guard_stop_restore_start_order_and_failures(self):
  source=SOURCE.read_text();a=source.index('static bool restore(void)');b=source.index('static bool boot_enabled',a)
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(PRELUDE+source[a:b]+CASES);subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],cwd=p,check=True)
if __name__=='__main__':unittest.main()
