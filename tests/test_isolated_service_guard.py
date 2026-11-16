"""Check that non-boot experiments refuse concurrent resident services."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'firmware/usb-resident/source/main.c'
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64; typedef uint32_t Result;
#define R_FAILED(rc) ((rc)!=0)
#define CUR_PROCESS_HANDLE 0
#define Module_Libnx 1
#define LibnxError_BadInput 2
#define LibnxError_NotInitialized 3
#define MAKERESULT(module,desc) ((module) + ((desc)<<9))
static int peer, pm_failure, self_failure, exits, calls;
static Result svcGetProcessId(u64 *out,int handle){(void)handle;*out=123;return self_failure?19:0;}
static Result pmdmntInitialize(void){return pm_failure?23:0;}
static void pmdmntExit(void){exits++;}
static Result pmdmntGetProcessId(u64 *out,u64 id){calls++;unsigned n=(unsigned)(id&15);
    if(peer==4 && n==12)return 29;
    if(peer==1 && n==11){*out=456;return 0;}
    if(peer==2 && n==13){*out=456;return 0;}
    if(peer!=3 && n==14){*out=123;return 0;}
    return 0x20f;
}
'''
CASES = r'''
int main(void) {
    assert(isolated_service_guard()==0 && exits==1);
    peer=1; assert(isolated_service_guard()==MAKERESULT(1,2) && exits==2);
    peer=2; assert(isolated_service_guard()==MAKERESULT(1,2) && exits==3);
    peer=3; assert(isolated_service_guard()==MAKERESULT(1,3) && exits==4);
    peer=4; assert(isolated_service_guard()==29 && exits==5);
    self_failure=1; int old_calls=calls;
    assert(isolated_service_guard()==19 && calls==old_calls && exits==5);
    self_failure=0; pm_failure=1;
    assert(isolated_service_guard()==23 && calls==old_calls && exits==5);
    return 0;
}
'''


class IsolatedGuardTest(unittest.TestCase):
    def test_peers_and_process_manager_errors(self):
        source = SOURCE.read_text()
        start = source.index('static Result isolated_service_guard(void) {')
        end = source.index('\n// Record the surrounding devices', start)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'guard.c'
            executable = Path(directory) / 'guard'
            path.write_text(PRELUDE + source[start:end] + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
