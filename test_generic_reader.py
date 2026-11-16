"""Exercise the actual reader dispatcher with different dmnt targets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

READER = (Path(__file__).parent / 'reader/source/main.c').resolve()

PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
typedef uint8_t u8; typedef uint64_t u64; typedef int Result;
typedef struct { u64 base, size; } Extents;
typedef struct { u64 process_id, title_id; Extents main, heap, alias, address_space; u8 build_id[32]; } Metadata;
typedef struct { u64 addr, size; unsigned type, perm; } MemoryInfo;
#define R_FAILED(x) ((x) != 0)
#define ZA_READER_RESIDENT 1
static int dmnt, mapping_failure, open_calls, open_failure, has_failure;
static bool active_process;
#define serviceDispatchOut(service, cmd, out) \
    ((void)(service), (void)(cmd), (out)=active_process, has_failure)
#define serviceDispatch(service, cmd) \
    ((void)(service), (void)(cmd), open_calls++, open_failure)
#define serviceDispatchInOut(service, cmd, address, info) \
    ((void)(service), (void)(cmd), (void)(address), \
     (info)=(MemoryInfo){.addr=0x5000,.size=mapping_failure?0:0x200,.type=3,.perm=3}, 0)
static u8 bytes[0x10000]; static char response[sizeof(bytes)*2+2], output[sizeof(response)];
static const u8 expected_build[8]={0xb1,0xf1,0x2f,0xd9,0x19,0xea,0xe8,0x6a};
static Metadata target; static int reads, game_calls, metadata_failure; static u64 last_address;
static bool send_all(int fd,const char *data,size_t n){(void)fd;memcpy(output,data,n);output[n]=0;return true;}
static bool error_reply(int fd,Result rc){(void)rc;return send_all(fd,"ERR native\n",11);}
static Result metadata(Metadata *m){if(metadata_failure)return 1;*m=target;return 0;}
static Result read_memory(u64 a,void *p,size_t n){reads++;last_address=a;memset(p,0xAB,n);return 0;}
static bool capture_templates(int fd,const Metadata *m){(void)fd;(void)m;game_calls++;return true;}
static bool reticle_names(int fd,const Metadata *m){(void)fd;(void)m;game_calls++;return true;}
static bool class_objects(int fd,u64 base,u64 desc){(void)fd;(void)base;(void)desc;game_calls++;return true;}
static u64 reader_started; static bool sleep_disabled; static Result sleep_set_result, sleep_query_result;
static Result appletIsAutoSleepDisabled(bool *p){(void)p;return 0;}
static u64 svcGetSystemTick(void){return 0;} static u64 armTicksToNs(u64 x){return x;}
'''

CASES = r'''
#include <assert.h>
int main(void) {
    target.process_id=42; target.title_id=0x0100000000001234ULL;
    target.main.base=0x1000; target.main.size=0x100;
    target.heap.base=0x5000; target.heap.size=0x200;
    target.build_id[0]=0xA5; target.build_id[31]=0x5A;
    active_process=true;
    assert(command(0,"openForeground") && !strcmp(output,"ALREADY OPEN\n") && open_calls==0);
    active_process=false; has_failure=1;
    assert(command(0,"openForeground") && !strcmp(output,"ERR native\n") && open_calls==0);
    has_failure=0; open_failure=1;
    assert(command(0,"openForeground") && !strcmp(output,"ERR native\n") && open_calls==1);
    open_failure=0;
    assert(command(0,"openForeground") && !strcmp(output,"OK\n") && open_calls==2);
    assert(command(0,"getTitleID") && !strcmp(output,"0100000000001234\n"));
    assert(command(0,"getProcessID") && !strcmp(output,"000000000000002A\n"));
    assert(command(0,"getBuildID") && !strcmp(output,"A500000000000000\n"));
    assert(command(0,"getFullBuildID") && strlen(output)==65 && output[0]=='A' && output[62]=='5' && output[63]=='A');
    assert(command(0,"getMainNsoSize") && !strcmp(output,"0000000000000100\n"));
    assert(command(0,"getHeapSize") && !strcmp(output,"0000000000000200\n"));
    assert(command(0,"queryMemory 0x5010") && !strcmp(output,"base=0000000000005000 size=0000000000000200 type=00000003 perm=00000003\n"));
    mapping_failure=1;
    assert(command(0,"queryMemory 0x5010") && !strcmp(output,"ERR mapping-layout\n"));
    mapping_failure=0;
    assert(command(0,"peekMain 0xFE 2") && !strcmp(output,"ABAB\n") && last_address==0x10FE);
    assert(command(0,"peekAbsolute 0x5010 1") && !strcmp(output,"AB\n") && last_address==0x5010);
    int count=reads;
    assert(command(0,"peekMain 0xFF 2") && !strcmp(output,"ERR outside-main\n") && reads==count);
    assert(command(0,"peekAbsolute 0xFFFFFFFFFFFFFFFF 1") && !strcmp(output,"ERR address-overflow\n") && reads==count);
    target.main.base=UINT64_MAX-4;
    assert(command(0,"peekMain 0x10 1") && !strcmp(output,"ERR address-overflow\n") && reads==count);
    const char *specific[]={"getActors","getTagComponents","findCaptureTemplates","findReticleNames","findClass 0x3CE2B18"};
    for(size_t i=0;i<sizeof(specific)/sizeof(*specific);i++)
        assert(command(0,specific[i]) && !strcmp(output,"ERR wrong-game-or-build\n") && game_calls==0);
    target.title_id=0x0100F43008C44000ULL; memcpy(target.build_id,expected_build,8);
    assert(command(0,"getActors") && game_calls==1);
    target.build_id[0]^=1;
    assert(command(0,"getActors") && !strcmp(output,"ERR wrong-game-or-build\n") && game_calls==1);
    metadata_failure=1;
    assert(command(0,"peekAbsolute 0x5010 1") && !strcmp(output,"ERR native\n") && reads==count);
    return 0;
}
'''


class GenericReaderTest(unittest.TestCase):
    def test_dispatcher_scope_and_bounds(self):
        src = READER.read_text()
        begin = src.index('static bool command(int fd, const char *line) {')
        end = src.index('\n#ifndef ZA_READER_RESIDENT\nint main(void)', begin)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'reader.c'
            executable = Path(directory) / 'reader-test'
            source.write_text(PRELUDE + src[begin:end] + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
