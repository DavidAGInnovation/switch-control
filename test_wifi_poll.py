"""Exercise the actual nonblocking handshake loop with local socket pairs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).parent / 'usb-resident/source/main.c'

PRELUDE = r'''
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
typedef uint8_t u8;
typedef uint64_t u64;
static int wifi_listener=1, pending[32],pending_n,pending_pos,dispatches;
static u8 wifi_key[32];
static int wifi_recovery;
static u64 ticks=1;
static u64 svcGetSystemTick(void){return ticks;}
static u64 armTicksToNs(u64 n){return n;}
static int fake_accept(int fd,void *a,void *b){(void)fd;(void)a;(void)b;if(pending_pos<pending_n)return pending[pending_pos++];errno=EAGAIN;return -1;}
#define accept fake_accept
static void randomGet(void *p,size_t n){memset(p,0xaa,n);}
static void hmacSha256CalculateMac(u8 *out,const u8 *k,size_t kn,const u8 *p,size_t n){(void)k;(void)kn;(void)p;(void)n;memset(out,0x5a,32);}
static void wifiRecoveryAuthenticated(int *p){(void)p;}
static bool send_all(int fd,const char *p,size_t n){return send(fd,p,n,0)==(ssize_t)n;}
static bool dispatch(int fd,const char *line){assert(!strcmp(line,"getReaderStatus"));dispatches++;return send_all(fd,"OK\n",3);}
'''

CASES = r'''
static void add_pair(int *client){int p[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p));pending[pending_n++]=p[0];*client=p[1];}
static void challenge(int fd){u8 b[32];assert(recv(fd,b,sizeof(b),MSG_WAITALL)==32);for(int i=0;i<32;i++)assert(b[i]==0xaa);}
static void request(int fd,bool valid){const char *s="getReaderStatus";size_t n=strlen(s);u8 b[2+255+32]={0};b[1]=(u8)n;memcpy(b+2,s,n);memset(b+2+n,valid?0x5a:0,32);assert(send(fd,b,3,0)==3);wifi_poll();assert(send(fd,b+3,2+n+32-3,0)==(ssize_t)(2+n+32-3));}
int main(void){
 signal(SIGPIPE,SIG_IGN);
 int idle,good;add_pair(&idle);add_pair(&good);wifi_poll();challenge(idle);challenge(good);
 request(good,true);wifi_poll();char reply[4]={0};assert(recv(good,reply,3,0)==3&&!strcmp(reply,"OK\n"));assert(dispatches==1);
 ticks+=2000000001ULL;wifi_poll();assert(recv(idle,reply,1,0)==0);close(idle);close(good);
 int bad;add_pair(&bad);wifi_poll();challenge(bad);request(bad,false);wifi_poll();assert(recv(bad,reply,1,0)==0&&dispatches==1);close(bad);
 int clients[9];for(int i=0;i<9;i++)add_pair(&clients[i]);wifi_poll();wifi_poll();
 for(int i=0;i<8;i++)challenge(clients[i]);
 assert(recv(clients[8],reply,1,0)==0);ticks+=2000000001ULL;wifi_poll();
 for(int i=0;i<9;i++)close(clients[i]);
 wifi_close_peers();return 0;
}
'''


class WifiPollTest(unittest.TestCase):
    def test_stalled_fragmented_invalid_and_excess_peers(self):
        source = SOURCE.read_text()
        start = source.index('// Each unauthenticated peer has an absolute deadline')
        end = source.index('int main(int argc,char **argv)', start)
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            cfile.write_text(PRELUDE + source[start:end] + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
