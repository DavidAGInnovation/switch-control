from pathlib import Path
import subprocess,tempfile,unittest
HERE=Path(__file__).parent
class MetadataTextTest(unittest.TestCase):
 def test_nacp_validation_rejects_binary_and_truncated_names(self):
  source=r'''
#include <assert.h>
#include <string.h>
#include "utf8-text.h"
int main(void){
 const char *good[]={"Pokémon Legends: Z-A","世界","🎮","Mario"};
 for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);i++)assert(utf8_text_valid(good[i],strlen(good[i])));
 const char *bad[]={"*\x01\xed\xd6","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","a\nb","\x80","\xe2x"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
  assert(!utf8_text_valid(bad[i],strlen(bad[i])));
 }
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(source);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(HERE/'usb-resident/source'),str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
