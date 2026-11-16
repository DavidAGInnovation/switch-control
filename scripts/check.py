#!/usr/bin/env python3
"""Run the complete host suite, rejecting unavailable TLS coverage or skips."""
import os
from pathlib import Path
import ssl
import subprocess
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
if not os.environ.get('ZA_MBEDTLS_HOST_ROOT'):
    raise SystemExit('Set ZA_MBEDTLS_HOST_ROOT to the pinned host-built Mbed TLS tree.')
if not getattr(ssl, 'HAS_PSK', False) or not hasattr(ssl.SSLContext, 'set_psk_server_callback'):
    raise SystemExit('Python 3.13+ with OpenSSL PSK support is required.')
subprocess.run([sys.executable, str(ROOT/'scripts/generate_schemas.py'), '--check'], check=True)
sys.path.insert(0, str(ROOT))
suite = unittest.defaultTestLoader.discover(str(ROOT), pattern='test_*.py')
result = unittest.TextTestRunner(verbosity=2).run(suite)
if result.skipped:
    print('Incomplete coverage: skipped tests are not allowed.', file=sys.stderr)
raise SystemExit(0 if result.wasSuccessful() and not result.skipped else 1)
