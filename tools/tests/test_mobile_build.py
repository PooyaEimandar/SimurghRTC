"""Build-input and toolchain integrity checks; no network or SDK is required."""
from pathlib import Path
import json
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

    def test_android_vtable_rejects_relative_or_missing_symbols(self):
        build.validate_android_vtable('0000000000000000 0000000000000080 D _ZTVN3rtc6ThreadE')
        for symbols in ('', '0000000000000000 0000000000000040 R _ZTVN3rtc6ThreadE'):
            with self.assertRaisesRegex(ValueError, 'Itanium'):
                build.validate_android_vtable(symbols)

    def test_simulator_selects_its_own_gn_target(self):
        args = build.gn_arguments(build.load_lock(), 'IOSSimulator', Path('/clang'))
        self.assertIn('target_os = "ios"', args)
        self.assertIn('target_environment = "simulator"', args)
        self.assertNotIn('target_environment = "device"', args)

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

    def test_fresh_build_refuses_to_reuse_outputs(self):
        directory, empty = build.build_directory(self.root, 'Android', 'repeat-a', fresh=True)
        self.assertTrue(empty)
        (directory / 'keep.o').write_bytes(b'prior compiler output')
        with self.assertRaisesRegex(ValueError, '--fresh requires an empty'):
            build.build_directory(self.root, 'Android', 'repeat-a', fresh=True)
        self.assertEqual((directory / 'keep.o').read_bytes(), b'prior compiler output')
        _, empty = build.build_directory(self.root, 'Android', 'repeat-a')
        self.assertFalse(empty)

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

    def repeat_pair(self):
        first, second = self.root / 'first', self.root / 'second'
        for directory in (first, second):
            make_payload(directory)
            (directory / 'Build/build-lock.json').write_text('{"test": true}')
            build.write_json(directory / 'Build/toolchain.json', {
                'compiler': 'test-only', 'python': 'test-only', 'sdk': 'test-only',
                'compiler_root': str(directory / 'clang'), 'host': str(directory),
                'build_directory': str(directory / 'out'), 'fresh_output': True})
            (directory / 'Build/args.gn').write_text(
                f'clang_base_path = {json.dumps(str(directory / "clang"))}\nis_debug = false\n')
            build.write_json(directory / 'Build/link-probe.json', {
                'status': 'linked', 'whole_archive': True, 'engine_libraries': [],
                'binary_sha256': str(directory)})
            seal(directory)
        return first, second

    def test_repeat_comparison_accepts_host_paths_but_rejects_different_headers(self):
        first, second = self.repeat_pair()
        report = build.compare_artifacts(first, second, require_fresh=True)
        self.assertEqual(report['status'], 'identical')
        self.assertTrue(report['fresh_outputs'])
        self.assertEqual(len(report['manifest_sha256']), 2)
        (second / 'Include/rtc_base/thread.h').write_text('a different header')
        seal(second)
        with self.assertRaisesRegex(ValueError, 'Build outputs differ'):
            build.compare_artifacts(first, second)

    def test_repeat_comparison_rejects_changed_archive_even_with_valid_manifest(self):
        first, second = self.repeat_pair()
        archive = second / 'Lib/libwebrtc.a'
        data = bytearray(archive.read_bytes())
        data[-1] = 1  # Change a non-identity byte in the synthetic object.
        archive.write_bytes(data)
        seal(second)
        with self.assertRaisesRegex(ValueError, 'Build outputs differ'):
            build.compare_artifacts(first, second)

    def test_repeat_comparison_rejects_observed_tool_or_flag_drift(self):
        for field in ('compiler', 'python', 'sdk', 'xcode', 'sdk_build', 'gn', 'ninja', 'recipe_sha256'):
            with self.subTest(field=field):
                first, second = self.repeat_pair()
                path = second / 'Build/toolchain.json'
                record = json.loads(path.read_text())
                record[field] = 'different'
                build.write_json(path, record)
                seal(second)
                with self.assertRaisesRegex(ValueError, 'Observed build inputs differ'):
                    build.compare_artifacts(first, second)
        first, second = self.repeat_pair()
        path = second / 'Build/args.gn'
        path.write_text(path.read_text().replace('is_debug = false', 'is_debug = true'))
        seal(second)
        with self.assertRaisesRegex(ValueError, 'Observed build inputs differ'):
            build.compare_artifacts(first, second)

    def test_repeat_comparison_requires_fresh_evidence_when_requested(self):
        for value in (False, None):
            first, second = self.repeat_pair()
            path = second / 'Build/toolchain.json'
            record = json.loads(path.read_text())
            if value is None:
                del record['fresh_output']
            else:
                record['fresh_output'] = value
            build.write_json(path, record)
            seal(second)
            self.assertFalse(build.compare_artifacts(first, second)['fresh_outputs'])
            with self.assertRaisesRegex(ValueError, 'fresh output directory'):
                build.compare_artifacts(first, second, require_fresh=True)

    def test_repeat_comparison_rejects_failed_link_probe(self):
        first, second = self.repeat_pair()
        build.write_json(second / 'Build/link-probe.json', {'status': 'failed', 'whole_archive': True})
        seal(second)
        with self.assertRaisesRegex(ValueError, 'successful whole-archive'):
            build.compare_artifacts(first, second)


if __name__ == '__main__':
    unittest.main()
