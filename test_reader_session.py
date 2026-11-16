"""Exercise the resident reader's actual service-handle lifecycle."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).parent / 'usb-resident/source/main.c'
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
typedef int Result; typedef struct { bool active; } Service;
#define R_FAILED(rc) ((rc)!=0)
static Service dmnt; static int opens, closes, commands, errors, open_error;
static bool command_result=true;
static bool serviceIsActive(Service*s){return s->active;}
static void serviceClose(Service*s){assert(s->active);s->active=false;closes++;}
static Result smGetService(Service*s,const char*n){assert(!strcmp(n,"dmnt:cht"));opens++;
    if(open_error){return open_error;}
    assert(!s->active);s->active=true;return 0;}
static bool error_reply(int fd,Result rc){(void)fd;assert(rc==17);errors++;return false;}
static bool command(int fd,const char*line){(void)fd;(void)line;assert(dmnt.active);commands++;return command_result;}
'''
CASES = r'''
int main(void) {
    assert(scoped_reader_command(3,"getTitleID"));
    assert(!dmnt.active && opens==1 && closes==1 && commands==1);
    command_result=false;
    assert(!scoped_reader_command(3,"peekAbsolute 0 1"));
    assert(!dmnt.active && opens==2 && closes==2 && commands==2);
    open_error=17;
    assert(!scoped_reader_command(3,"getTitleID"));
    assert(!dmnt.active && opens==3 && closes==2 && commands==2 && errors==1);
    open_error=0; command_result=true; dmnt.active=true;
    assert(scoped_reader_command(3,"getBuildID"));
    assert(!dmnt.active && opens==4 && closes==4 && commands==3);
    return 0;
}
'''


class ReaderSessionTest(unittest.TestCase):
    def test_session_is_closed_on_all_dispatch_paths(self):
        source = SOURCE.read_text()
        start = source.index('static bool scoped_reader_command(')
        end = source.index('\n// Release the capture service', start)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'session.c'
            executable = Path(directory) / 'session'
            path.write_text(PRELUDE + source[start:end] + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
