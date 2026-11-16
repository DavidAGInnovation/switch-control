#!/usr/bin/env python3
"""Build the public SD install package from this standalone source tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
IMAGE = 'devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282'
VERSION = '0.5.1'

def assemble(output, mbed):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = output / 'native'
    subprocess.run([sys.executable, str(ROOT/'scripts/build_tls.py'), '--mbedtls-root',
                    str(Path(mbed).resolve()), str(build)], check=True)
    setup = output / 'setup'
    shutil.copytree(ROOT/'firmware/setup', setup, ignore=shutil.ignore_patterns(
        'build', '*.nro', '*.nacp', '*.elf', '*.map', '__pycache__'))
    subprocess.run(['docker', 'run', '--rm', '-u', f'{os.getuid()}:{os.getgid()}',
                    '-v', f'{setup}:/app', '-w', '/app', IMAGE, 'make', '-j4'], check=True)
    release = output / f'switch-control-{VERSION}.zip'
    subprocess.run([sys.executable, str(ROOT/'scripts/package_release.py'),
                    str(build/'usb-resident/usb-control.nsp'),
                    str(setup/'switch-control-setup.nro'), str(build/'build.json'),
                    str(release)], check=True)
    result = {'release': release.name,
              'service_sha256': json.loads((build/'build.json').read_text())['service_sha256'],
              'zip_sha256': hashlib.sha256(release.read_bytes()).hexdigest()}
    (output/'release.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--mbedtls-root', required=True, type=Path)
    args = parser.parse_args()
    assemble(args.output, args.mbedtls_root)
