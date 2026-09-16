import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release', ROOT/'tools/release_firmware.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(release.version('v1.2.3-rc.1'), '1.2.3-rc.1')
        for bad in ('', '../1.2.3', '1.2.3\n', '$(whoami)', '1.2.3;echo', '01.2.3', '1.2', '1.2.3-debug'):
            with self.assertRaises(ValueError):
                release.version(bad)
        self.assertEqual(release.select_version('', '0.6.14', ['v0.6.5']), '0.6.14')
        self.assertEqual(release.select_version('', '0.6.14', ['v0.6.14', 'v0.6.15']), '0.6.16')
        self.assertEqual(release.select_version('', '0.6.14', ['v1.0.0']), '1.0.1')
        self.assertEqual(release.select_version('', '0.6.14', ['v0.6.15-rc.1']), '0.6.14')
        with self.assertRaises(ValueError):
            release.select_version('v0.6.14', '0.6.14', ['0.6.14'])

    def test_package_and_debug_rejection(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)/'build'
            build.mkdir()
            (build/'build-config.txt').write_text('-DFW_VERSION=1.2.3')
            for ext in ('bin', 'hex', 'elf', 'map'):
                (build/f'ch582f.{ext}').write_bytes(b'firmware1.2.3\0')
            (build/'private-recording.wav').write_bytes(b'never package me')
            out = Path(temp)/'release'
            release.package('1.2.3', build, out)
            manifest = json.loads((out/'assets/manifest.json').read_text())
            self.assertFalse(manifest['debug'])
            for line in (out/'assets/SHA256SUMS.txt').read_text().splitlines():
                sha, name = line.split('  ')
                self.assertEqual(sha, hashlib.sha256((out/'assets'/name).read_bytes()).hexdigest())
            with zipfile.ZipFile(out/'assets/ch582f-1.2.3.zip') as z:
                self.assertNotIn('private-recording.wav', z.namelist())
                self.assertIn('THIRD_PARTY_NOTICES.md', z.namelist())
            with self.assertRaises(ValueError):
                release.package('1.2.3', build, out)
            for define in ('-DRBP_DEBUG', '-DRBP_SDK_RX_PROBE', '-DRBP_EXPERIMENTAL_HOST_VOICE_START'):
                (build/'build-config.txt').write_text(define)
                with self.assertRaises(ValueError):
                    release.package('1.2.3', build, Path(temp)/'bad')


if __name__ == '__main__':
    unittest.main()
