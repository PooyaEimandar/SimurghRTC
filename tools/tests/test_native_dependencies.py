"""Dependency provenance must not silently accept drift or misleading version paths."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from audit_native_dependencies import check_libraries, file_record
from mobile_artifacts import digest


class NativeDependencyTests(unittest.TestCase):
    def test_detects_changed_external_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lib = root / 'lib.a'
            lib.write_bytes(b'pinned library')
            spec = {'path': 'lib.a', 'sha256': digest(lib)}
            self.assertEqual(check_libraries(root, [spec])['lib.a']['sha256'], spec['sha256'])
            lib.write_bytes(b'changed library')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                check_libraries(root, [spec])

    def test_reports_embedded_version_without_inferring_from_folder(self):
        with tempfile.TemporaryDirectory(prefix='opus-1.1-') as temporary:
            path = Path(temporary) / 'lib.a'
            path.write_bytes(b'\0libopus unknown-fixed\0libopus 1.1-beta\0OpenSSL 1.1.1t  7 Feb 2023\0')
            versions = file_record(path, versions=True)['version_strings']
            self.assertEqual(versions, ['OpenSSL 1.1.1t  7 Feb 2023', 'libopus 1.1-beta', 'libopus unknown-fixed'])

    def test_rejects_missing_and_unsafe_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ('missing.a', '../outside.a', '/outside.a'):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    check_libraries(root, [{'path': name, 'sha256': '0' * 64}])


if __name__ == '__main__':
    unittest.main()
