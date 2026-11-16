#!/usr/bin/env python3
"""Archive the exact tracked prepared Mbed TLS sources, including its submodule."""
import argparse
import io
from pathlib import Path
import subprocess
import tarfile

COMMIT = '068ff080b369adfac81509f9b57b2afabaf82dc5'
FRAMEWORK = 'dde0c4a0e448a0552f18817dcea633bb851fd288'
NOTICE = b'/* Modified for Switch Control: platform entropy/time and custom socket I/O. */\n'

def archive(root, output):
    root = root.resolve()
    if output.exists():
        raise FileExistsError(output)
    with tarfile.open(output, 'w:gz') as tar:
        for directory, commit, prefix in [(root, COMMIT, 'mbedtls/'),
                                         (root/'framework', FRAMEWORK, 'mbedtls/framework/')]:
            actual = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != commit:
                raise ValueError('Unexpected dependency revision')
            names = subprocess.check_output(['git', '-C', str(directory), 'ls-files', '-z']).decode().split('\0')
            for name in sorted(filter(None, names)):
                path = directory/name
                if not path.is_file():
                    continue
                data = path.read_bytes()
                if name == 'include/mbedtls/mbedtls_config.h' and not data.startswith(NOTICE):
                    data = NOTICE+data
                info = tarfile.TarInfo(prefix+name)
                info.size = len(data)
                info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                info.mtime = 0
                tar.addfile(info, io.BytesIO(data))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mbedtls_root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    archive(args.mbedtls_root, args.output)
