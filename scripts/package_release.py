#!/usr/bin/env python3
"""Package reviewed sources and SD binaries, with a manifest for every file."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.5.0'

def source_files():
    """Explicit source allowlist; deployment evidence and build outputs stay out."""
    for name in ['LICENSE', 'LICENSE-EXCEPTION', 'README.md', 'SECURITY.md',
                 'CONTRIBUTING.md', '.gitignore']:
        yield ROOT/name
    for directory in ['firmware', 'switch_control', 'tests', 'docs', 'licenses',
                      'scripts', '.github']:
        for path in (ROOT/directory).rglob('*'):
            if not path.is_file() or any(p in {'build', '__pycache__', '.git'} for p in path.parts):
                continue
            if (directory == 'licenses' or path.name in {'Makefile', 'LICENSE'} or
                    path.suffix in {'.c', '.h', '.json', '.sh', '.py', '.yml', '.md'}):
                if path.name != 'payloads.h':
                    yield path

def package(service, setup, build_info, output):
    output = Path(output)
    if output.exists():
        raise FileExistsError(output)
    entries = {
        'sd/atmosphere/contents/430000000000000D/exefs.nsp': Path(service).read_bytes(),
        'sd/atmosphere/contents/430000000000000D/flags/boot2.flag': b'',
        'optional/switch-control-setup.nro': Path(setup).read_bytes(),
        'README.md': (b'The build/client commands below run from the included source/ directory\n'
                      b'or a repository checkout.\n\n'+(ROOT/'README.md').read_bytes()),
        'docs/RELEASE.md': (ROOT/'docs/RELEASE.md').read_bytes(),
        'SECURITY.md': (ROOT/'SECURITY.md').read_bytes(),
        'LICENSE': (ROOT/'LICENSE').read_bytes(),
        'LICENSE-EXCEPTION': (ROOT/'LICENSE-EXCEPTION').read_bytes(),
        'docs/THIRD_PARTY.md': (ROOT/'docs/THIRD_PARTY.md').read_bytes(),
        'build/service.json': Path(build_info).read_bytes(),
    }
    for path in ROOT.joinpath('licenses').iterdir():
        if path.is_file():
            entries['licenses/'+path.name] = path.read_bytes()
    for path in source_files():
        entries['source/'+path.relative_to(ROOT).as_posix()] = path.read_bytes()
    for path in (ROOT/'switch_control').glob('*.py'):
        entries['client/switch_control/'+path.name] = path.read_bytes()
    manifest = {'version': VERSION,
                'service_sha256': hashlib.sha256(Path(service).read_bytes()).hexdigest(),
                'files': {name: hashlib.sha256(data).hexdigest()
                          for name, data in sorted(entries.items())}}
    entries['manifest.json'] = (json.dumps(manifest, indent=2)+'\n').encode()
    for name in entries:
        if name.endswith(('.key', '.pem', '.der', '.token', '.jpg')):
            raise ValueError('Private file in release')
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 16, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name.endswith('.sh') else 0o644) << 16
            archive.writestr(info, data)
    return manifest

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['service', 'setup', 'build_info', 'output']:
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = package(args.service, args.setup, args.build_info, args.output)
    print(json.dumps({'version': result['version'], 'files': len(result['files'])}))
