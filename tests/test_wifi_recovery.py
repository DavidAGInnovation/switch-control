"""Exercise network transition policy used by the resident listener."""
from pathlib import Path
import subprocess
import tempfile
import unittest

HEADER = Path(__file__).resolve().parents[1] / 'firmware/usb-resident/source/wifi-recovery.h'
CASES = r'''
#include <assert.h>
#include "wifi-recovery.h"
int main(void) {
    WifiRecovery s={0};
    assert(!wifiRecoveryReady(&s,true));
    wifiRecoveryFault(&s);
    assert(!wifiRecoveryReady(&s,false));
    assert(wifiRecoveryReady(&s,true));
    assert(!wifiRecoveryReady(&s,true)); // Failed bind is not blindly retried.
    assert(!wifiRecoveryReady(&s,false));
    assert(wifiRecoveryReady(&s,true)); // Actual network transition permits one attempt.
    wifiRecoveryOpened(&s);
    assert(!wifiRecoveryReady(&s,true));
    wifiRecoveryFault(&s);
    assert(!wifiRecoveryReady(&s,true)); // No rapid fault/rebind loop.
    wifiRecoveryAuthenticated(&s);
    assert(wifiRecoveryReady(&s,true));
    wifiRecoveryOpened(&s);
    wifiRecoveryAuthenticated(&s);
    wifiRecoveryFault(&s);
    assert(wifiRecoveryReady(&s,true)); // A later independent fault can recover.
    return 0;
}
'''


class WifiRecoveryTest(unittest.TestCase):
    def test_fault_and_network_transitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            source.write_text(CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(HEADER.parent), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
