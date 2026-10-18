"""Build-input and toolchain integrity checks; no network or SDK is required."""
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mobile_build as build
from test_mobile_artifacts import make_payload, seal


class MobileBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def archive(self, entries):
        path = self.root / 'tools.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            for name, data, mode in entries:
                info = zipfile.ZipInfo(name)
                info.external_attr = mode << 16
                archive.writestr(info, data)
        return path

    def test_pins_and_recorded_patch_hashes_are_valid(self):
        lock = build.load_lock()
        self.assertEqual(lock['repositories']['src']['revision'],
                         '4e8a5ac68e8a4ae0588f54f2fdb8cbd1eb5fa50d')
        for spec in lock['tools'].values():
            self.assertEqual(len(spec['sha256']), 64)

    def test_workspace_never_adopts_existing_files(self):
        (self.root / 'keep.txt').write_text('user data')
        with self.assertRaisesRegex(ValueError, 'never adopted'):
            with build.workspace(self.root):
                pass
        self.assertEqual((self.root / 'keep.txt').read_text(), 'user data')

    def test_workspace_is_exclusive(self):
        with build.workspace(self.root):
            with self.assertRaisesRegex(ValueError, 'Another operation'):
                with build.workspace(self.root):
                    pass

    def test_generated_paths_reject_symlinked_parents(self):
        outside = self.root / 'outside'
        outside.mkdir()
        work = self.root / 'work'
        work.mkdir()
        (work / 'host').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Symlink in generated'):
            build.owned_path(work, 'host/ndk-r27c')

    def test_checksum_failure_does_not_replace_download(self):
        path = self.root / 'archive.zip'
        path.write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'corrupt'):
            build.download({'sha256': '0' * 64}, path)
        self.assertEqual(path.read_bytes(), b'corrupt')

    def test_zip_rejects_parent_escape(self):
        archive = self.archive([('../escape', b'bad', stat.S_IFREG | 0o644)])
        with self.assertRaises(ValueError):
            build.extract_zip(archive, self.root / 'extracted')
        self.assertFalse((self.root / 'escape').exists())

    def test_zip_accepts_ndk_names_with_spaces(self):
        archive = self.archive([('setuptools/script (dev).tmpl', b'template', stat.S_IFREG | 0o644)])
        build.extract_zip(archive, self.root / 'extracted')
        self.assertEqual((self.root / 'extracted/setuptools/script (dev).tmpl').read_bytes(), b'template')

    def test_zip_rejects_symlink_escape(self):
        archive = self.archive([('bin/clang', b'../../escape', stat.S_IFLNK | 0o777)])
        with self.assertRaisesRegex(ValueError, 'symlink escapes'):
            build.extract_zip(archive, self.root / 'extracted')

    def test_zip_never_writes_through_existing_symlink(self):
        dest = self.root / 'extracted'
        dest.mkdir()
        outside = self.root / 'outside'
        outside.mkdir()
        (dest / 'bin').symlink_to(outside, target_is_directory=True)
        archive = self.archive([('bin/clang', b'bad', stat.S_IFREG | 0o755)])
        with self.assertRaises(ValueError):
            build.extract_zip(archive, dest)
        self.assertFalse((outside / 'clang').exists())

    def test_zip_internal_links_and_cached_toolchain_integrity(self):
        archive = self.archive([('bin/clang', b'compiler', stat.S_IFREG | 0o755),
                                ('bin/clang++', b'clang', stat.S_IFLNK | 0o777)])
        dest = self.root / 'extracted'
        build.extract_zip(archive, dest)
        (dest / '.archive-sha256').write_text('fixture')
        build.verify_extraction(archive, dest)
        self.assertEqual((dest / 'bin/clang++').read_bytes(), b'compiler')
        (dest / 'bin/clang').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'file drift'):
            build.verify_extraction(archive, dest)

    def test_cached_toolchain_rejects_extra_files(self):
        archive = self.archive([('bin/clang', b'compiler', stat.S_IFREG | 0o755)])
        dest = self.root / 'extracted'
        build.extract_zip(archive, dest)
        (dest / '.archive-sha256').write_text('fixture')
        (dest / 'extra.h').write_text('unexpected')
        with self.assertRaisesRegex(ValueError, 'Unexpected files'):
            build.verify_extraction(archive, dest)

    def test_host_and_python_mismatch_fail_before_build(self):
        lock = build.load_lock()
        with patch.object(build.platform, 'system', return_value='WrongOS'):
            with self.assertRaisesRegex(ValueError, 'requires'):
                build.preflight(lock, 'IOS', Path(sys.executable))
        with patch.object(build.platform, 'system', return_value='Darwin'), \
             patch.object(build.platform, 'machine', return_value='arm64'), \
             patch.object(build, 'run', return_value='0.0.0'):
            with self.assertRaisesRegex(ValueError, 'Expected Python'):
                build.preflight(lock, 'IOS', Path(sys.executable))

    def repository(self):
        repo = self.root / 'src'
        repo.mkdir()
        def git(*args):
            return build.run(['git', *args], cwd=repo)
        git('init', '-q')
        git('config', 'user.name', 'Fixture')
        git('config', 'user.email', 'fixture@example.invalid')
        git('config', 'commit.gpgSign', 'false')
        git('config', 'core.hooksPath', '/dev/null')
        git('remote', 'add', 'origin', 'https://example.invalid/src')
        (repo / 'input.h').write_text('original')
        git('add', 'input.h')
        git('commit', '-qm', 'fixture')
        lock = {'repositories': {'src': {'revision': git('rev-parse', 'HEAD'),
                                         'url': 'https://example.invalid/src'}}, 'patches': []}
        return repo, lock

    def test_source_edits_and_untracked_headers_are_rejected(self):
        repo, lock = self.repository()
        build.verify_source(self.root, lock)
        (repo / 'unexpected.h').write_text('added')
        with self.assertRaisesRegex(ValueError, 'Unexpected source files'):
            build.verify_source(self.root, lock)
        (repo / 'unexpected.h').unlink()
        (repo / 'input.h').write_text('edited')
        with self.assertRaisesRegex(ValueError, 'Unexpected source edits'):
            build.verify_source(self.root, lock)

    def test_fetch_refuses_to_reset_another_revision(self):
        repo, lock = self.repository()
        lock['repositories']['src']['revision'] = '0' * 40
        with patch.object(build, 'free_space'):
            with self.assertRaisesRegex(ValueError, 'another revision'):
                build.fetch(self.root, lock)
        self.assertEqual((repo / 'input.h').read_text(), 'original')

    def test_repeat_comparison_accepts_different_logs_but_rejects_different_headers(self):
        first, second = self.root / 'first', self.root / 'second'
        for directory in (first, second):
            make_payload(directory)
            (directory / 'Build/build-lock.json').write_text('{"test": true}')
            (directory / 'Build/toolchain.json').write_text(str(directory))
            seal(directory)
        self.assertEqual(build.compare_artifacts(first, second)['status'], 'identical')
        (second / 'Include/rtc_base/thread.h').write_text('a different header')
        seal(second)
        with self.assertRaisesRegex(ValueError, 'Build outputs differ'):
            build.compare_artifacts(first, second)

    def test_repeat_comparison_rejects_changed_archive_even_with_valid_manifest(self):
        first, second = self.root / 'first', self.root / 'second'
        for directory in (first, second):
            make_payload(directory)
            (directory / 'Build/build-lock.json').write_text('{"test": true}')
            seal(directory)
        archive = second / 'Lib/libwebrtc.a'
        data = bytearray(archive.read_bytes())
        data[-1] = 1  # Change a non-identity byte in the synthetic object.
        archive.write_bytes(data)
        seal(second)
        with self.assertRaisesRegex(ValueError, 'Build outputs differ'):
            build.compare_artifacts(first, second)


if __name__ == '__main__':
    unittest.main()
