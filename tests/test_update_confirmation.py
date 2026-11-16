"""Confirm the real updater handshake with Horizon-style inherited O_NONBLOCK."""
from pathlib import Path
import hashlib,hmac,shlex,socket,subprocess,tempfile,unittest
SOURCE=Path(__file__).resolve().parents[1]/'firmware/file-update/source/main.c'
PRELUDE=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <poll.h>
#include <openssl/hmac.h>
static int inherited_accept(int s,void *a,void *b){int fd=accept(s,a,b);if(fd>=0)fcntl(fd,F_SETFL,O_NONBLOCK);return fd;}
#define accept inherited_accept
static void randomGet(void *b,size_t n){memset(b,0xa5,n);}
static void hmacSha256CalculateMac(void *out,const void *key,size_t kn,const void *p,size_t n){unsigned len;HMAC(EVP_sha256(),key,(int)kn,p,n,out,&len);}
'''
MAIN=r'''
int main(void){int s=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};if(bind(s,(void*)&a,sizeof(a))||listen(s,1))return 2;socklen_t n=sizeof(a);getsockname(s,(void*)&a,&n);printf("%u\n",ntohs(a.sin_port));fflush(stdout);unsigned char key[32];memset(key,0x42,32);struct pollfd p={.fd=s,.events=POLLIN};if(poll(&p,1,5000)<=0)return 3;int result=decision(s,key);close(s);return result==1?0:result==-1?4:5;}
'''
class ConfirmationTest(unittest.TestCase):
 def test_confirmation_and_rollback_after_inherited_nonblocking_accept(self):
  source=SOURCE.read_text();a=source.index('static int decision(');b=source.index('int main(void)',a)
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.c').write_text(PRELUDE+source[a:b]+MAIN);flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True));subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p/'test.c'),*flags,'-o',str(p/'test')],check=True)
   for operation,key,expected,code in [(b'commit\0\0',bytes([0x42])*32,b'COMMIT\n',0),(b'rollback',bytes([0x42])*32,b'ROLLBACK\n',4),(b'commit\0\0',bytes([0x41])*32,b'REJECTED\n',5)]:
    server=subprocess.Popen([str(p/'test')],stdout=subprocess.PIPE)
    try:
     port=int(server.stdout.readline())
     with socket.create_connection(('127.0.0.1',port),timeout=3) as s:
      nonce=s.recv(32);self.assertEqual(len(nonce),32);s.sendall(operation+hmac.digest(key,nonce+operation,hashlib.sha256));self.assertEqual(s.recv(256),expected)
     self.assertEqual(server.wait(timeout=5),code)
    finally:
     if server.poll() is None:server.kill();server.wait()
     server.stdout.close()
if __name__=='__main__':unittest.main()
