"""Check the public download is complete, verifiable and free of local evidence."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import package_release

class ReleasePackageTest(unittest.TestCase):
    def test_manifest_install_tree_and_private_file_exclusion(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for name in ['LICENSE','LICENSE-EXCEPTION','README.md','RELEASE.md','THIRD_PARTY.md','SECURITY.md','CONTRIBUTING.md','.gitignore']:
                (root/name).write_text(name)
            for name in ['native_client.py','discovery.py','wifi_controller.py','wifi_tls_controller.py','sd_files.py','control.py']:
                (root/name).write_text('# client source\n')
            (root/'licenses').mkdir();(root/'licenses/notice').write_text('retained')
            (root/'usb-resident/source').mkdir(parents=True)
            (root/'usb-resident/source/main.c').write_text('source')
            (root/'usb-resident/source/private.key').write_bytes(b'never publish')
            (root/'local-deployment.json').write_text('private evidence')
            (root/'screen.jpg').write_bytes(b'private image')
            service=root/'service.nsp';service.write_bytes(b'service executable')
            setup=root/'setup.nro';setup.write_bytes(b'optional helper')
            metadata=root/'build.json';metadata.write_text('{}')
            output=root/'release.zip'
            with patch.object(package_release,'ROOT',root):
                package_release.package(service,setup,metadata,output)
            with zipfile.ZipFile(output) as archive:
                names=archive.namelist()
                self.assertEqual(len(names),len(set(names)))
                manifest=json.loads(archive.read('manifest.json'))
                self.assertEqual(set(manifest['files']),set(names)-{'manifest.json'})
                for name,digest in manifest['files'].items():
                    self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(),digest)
                    self.assertNotIn('..',Path(name).parts)
                self.assertEqual(archive.read('sd/atmosphere/contents/430000000000000D/exefs.nsp'),service.read_bytes())
                self.assertEqual(archive.read('optional/switch-control-setup.nro'),setup.read_bytes())
                self.assertFalse(any(n.endswith(('.key','.jpg','-deployment.json')) for n in names))
                self.assertFalse(any(n.startswith('sd/switch/') for n in names))
                self.assertIn('source/usb-resident/source/main.c',names)
