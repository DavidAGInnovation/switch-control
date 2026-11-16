from pathlib import Path
import json,os,socket,struct,subprocess,tempfile,threading,unittest
from switch_control.discovery import discover,parse_records,name_bytes
HERE=Path(__file__).resolve().parents[1]
PRE=r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
typedef uint32_t u32;typedef uint64_t u64;
'''
class DiscoveryStickTest(unittest.TestCase):
 def test_stick_bounds_and_malformed_numbers(self):
  text=PRE+'\n#include "stick.h"\n'+r'''int main(void){char s;int x,y;unsigned t;assert(stick_parse("stick L -32767 32767 1000",&s,&x,&y,&t)&&s=='L'&&x==-32767&&y==32767&&t==1000);const char *bad[]={"stick L 32768 0 50","stick L 0 0 0","stick R 0 0 1001","stick Q 0 0 50","stick L 1.2 0 50","stick L 99999999999999999999 0 50","stick L 0 0 50 trailing"};for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!stick_parse(bad[i],&s,&x,&y,&t));return 0;}'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(text);subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(HERE/'firmware/usb-resident/source'),str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
 def test_actual_native_dns_sd_response_and_malformed_packets(self):
  source=(HERE/'firmware/usb-resident/source/discovery.h').read_text();a=source.index('static unsigned disc_u16');b=source.index('static bool disc_identity',a)
  text=PRE+'\n#define DISC_SERVICE "_switch-control._tcp.local"\nstatic u32 disc_ip;static char disc_host[64]="switch-unit.local",disc_instance[96]="Switch Control unit._switch-control._tcp.local";\n'+source[a:b]+r'''
int main(void){unsigned char query[1500],out[1400];size_t n=fread(query,1,sizeof(query),stdin);disc_ip=htonl(INADDR_LOOPBACK);size_t len=disc_response(query,n,out,true);if(len)fwrite(out,1,len,stdout);return 0;}
'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(text);subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
   query=struct.pack('!6H',123,0,1,0,0,0)+name_bytes('_switch-control._tcp.local')+struct.pack('!HH',12,0x8001)
   packet=subprocess.check_output([str(p/'test')],input=query);ident,rows=parse_records(packet);self.assertEqual(ident,123);self.assertEqual(len(rows),4);self.assertTrue(any(kind==33 and value['port']==6012 for _,kind,_,value in rows));self.assertTrue(any(kind==1 and value=='127.0.0.1' for _,kind,_,value in rows))
   for bad in [b'',query[:15],struct.pack('!6H',0,0,1,0,0,0)+b'\xc0\x0c'+struct.pack('!HH',12,1)]:self.assertEqual(subprocess.check_output([str(p/'test')],input=bad),b'')
   server=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);server.bind(('127.0.0.1',0));port=server.getsockname()[1]
   def respond():
    with server:
     request,peer=server.recvfrom(1500);reply=subprocess.check_output([str(p/'test')],input=request);server.sendto(reply,peer)
   thread=threading.Thread(target=respond);thread.start();found=discover(.2,destination=('127.0.0.1',port));thread.join(2);self.assertEqual(found[0]['host'],'127.0.0.1');self.assertEqual(found[0]['properties']['auth'],'tls-psk')
if __name__=='__main__':unittest.main()
