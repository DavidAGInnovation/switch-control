"""Check the resident screenshot service is released before each reply."""
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
#include <stdio.h>
#include <inttypes.h>
typedef uint64_t u64; typedef int Result;
#define R_FAILED(rc) ((rc)!=0)
#define ViLayerStack_Screenshot 1
static char jpeg[64]; static int active, inits, exits, errors, sends;
static int init_error, capture_error, send_failure; static u64 returned_size=12;
static Result capsscInitialize(void){inits++;if(init_error)return 11;assert(!active);active=1;return 0;}
static void capsscExit(void){assert(active);active=0;exits++;}
static Result capsscCaptureJpegScreenShot(u64*size,void*buffer,size_t capacity,int stack,u64 timeout){
    (void)buffer;(void)capacity;(void)stack;(void)timeout;assert(active);*size=returned_size;return capture_error?12:0;}
static bool error_reply(int fd,Result rc){(void)fd;assert(!active);assert(rc==11||rc==12);errors++;return false;}
static bool send_all(int fd,const char*data,size_t size){(void)fd;(void)data;(void)size;assert(!active);sends++;return !send_failure;}
'''
CASES = r'''
int main(void) {
    assert(scoped_screenshot(3));
    assert(inits==1 && exits==1 && sends==2 && !active);
    capture_error=1;
    assert(!scoped_screenshot(3));
    assert(inits==2 && exits==2 && errors==1 && !active);
    capture_error=0; init_error=1;
    assert(!scoped_screenshot(3));
    assert(inits==3 && exits==2 && errors==2 && !active);
    init_error=0; returned_size=65;
    assert(scoped_screenshot(3));
    assert(inits==4 && exits==3 && sends==3 && !active);
    returned_size=12; send_failure=1;
    assert(!scoped_screenshot(3));
    assert(inits==5 && exits==4 && sends==4 && !active);
    return 0;
}
'''


class CaptureSessionTest(unittest.TestCase):
    def test_capture_handle_is_released_before_replies(self):
        source = SOURCE.read_text()
        start = source.index('static bool scoped_screenshot(')
        end = source.index('\nstatic bool dispatch(', start)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.c'
            executable = Path(directory) / 'capture'
            path.write_text(PRELUDE + source[start:end] + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
