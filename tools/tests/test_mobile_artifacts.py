"""Synthetic archive/manifest cases; these fixtures are not usable libwebrtc binaries."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('mobile_artifacts', Path(__file__).parents[1] / 'mobile_artifacts.py')
artifacts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(artifacts)
SPEC = importlib.util.spec_from_file_location('prepare_profile', Path(__file__).parents[1] / 'prepare_profile.py')
profiles = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profiles)


def metadata(platform='Android'):
    return {'schema': 1, 'artifact_id': f'test-only-{platform.lower()}', 'platform': platform,
            'architecture': 'arm64', 'engine_version': '5.8.3', 'adapter': artifacts.ADAPTER,
            'configuration': 'Release', 'dependencies': 'unreal-openssl-opus-vpx',
            'source_revision': '0123456789abcdef0123456789abcdef01234567',
            'toolchain': {'host': 'Linux' if platform == 'Android' else 'Mac', 'compiler': 'test-only',
                          'sdk': 'test-only', 'minimum_os': '26' if platform == 'Android' else '15.0',
                          'cpp_runtime': 'libc++'},
            'codecs': {'audio': ['opus'], 'video': ['VP8']}}


def elf(machine=183):
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<HH', data, 16, 1, machine)
    return bytes(data)


def macho(platform=2, cpu=0x100000C):
    return struct.pack('<8I', 0xFEEDFACF, cpu, 0, 1, 1, 24, 0, 0) + struct.pack('<6I', 0x32, 24, platform, 0, 0, 0)


def archive(*members):
    result = b'!<arch>\n'
    for name, data in members:
        result += f'{name:<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n'.encode('ascii')
        result += data + (b'\n' if len(data) % 2 else b'')
    return result


def make_payload(root, platform='Android'):
    for name in artifacts.REQUIRED:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('Synthetic test fixture; not a WebRTC artifact.\n')
    (root / 'Lib/libwebrtc.a').write_bytes(archive(('probe.o/', elf() if platform == 'Android' else macho())))
    (root / 'Build/source-lock.json').write_text(json.dumps({
        'webrtc_revision': metadata(platform)['source_revision'],
        'dependencies': {'test-only': 'abcdef01' * 5}, 'patches': []}))


def seal(root, platform='Android', meta=None):
    document = {'metadata': meta or metadata(platform),
                'files': {name: artifacts.digest(path) for name, path in artifacts.payload_files(root).items()}}
    (root / artifacts.MANIFEST).write_text(json.dumps(document, indent=2) + '\n')


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'artifact'
        make_payload(self.root)
        seal(self.root)

    def test_valid_platforms_and_honest_validation_scope(self):
        for platform in ['Android', 'IOS']:
            with self.subTest(platform=platform):
                make_payload(self.root, platform)
                seal(self.root, platform)
                report = artifacts.validate(self.root)
                self.assertEqual(platform, report['platform'])
                self.assertEqual(1, report['objects'])
                self.assertEqual('unverified', report['runtime_validation'])

    def test_package_inventory_and_no_overwrite(self):
        (self.root / artifacts.MANIFEST).unlink()
        meta = self.root.parent / 'metadata.json'
        meta.write_text(json.dumps(metadata()))
        output = self.root.parent / 'packaged'
        artifacts.package(self.root, meta, output)
        self.assertEqual(1, artifacts.validate(output)['objects'])
        with self.assertRaisesRegex(ValueError, 'must not exist'):
            artifacts.package(self.root, meta, output)
        with self.assertRaisesRegex(ValueError, 'outside'):
            artifacts.package(self.root, meta, self.root / 'nested')

    def test_corrupt_payload_is_rejected(self):
        for name in ['Lib/libwebrtc.a', 'Include/rtc_base/thread.h', 'Licenses/THIRD_PARTY_NOTICES.txt']:
            path = self.root / name
            before = path.read_bytes()
            path.write_bytes(before + b'changed')
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                artifacts.validate(self.root)
            path.write_bytes(before)

    def test_missing_extra_and_empty_payloads(self):
        path = self.root / 'unexpected.h'
        path.write_text('extra')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            artifacts.validate(self.root)
        path.unlink()
        path = self.root / 'Include/rtc_base/thread.h'
        path.unlink()
        seal(self.root)
        with self.assertRaisesRegex(ValueError, 'missing'):
            artifacts.validate(self.root)
        path.write_bytes(b'')
        seal(self.root)
        with self.assertRaisesRegex(ValueError, 'Empty'):
            artifacts.validate(self.root)

    def test_wrong_engine_adapter_target_and_dependencies(self):
        for field, value in [('schema', True), ('engine_version', '5.8.2'), ('adapter', 'current-upstream'),
                             ('architecture', 'x86_64'), ('platform', 'Mac'), ('configuration', 'Debug'),
                             ('dependencies', 'boringssl'), ('source_revision', 'main')]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                meta = metadata()
                meta[field] = value
                artifacts.validate_metadata(meta)

    def test_missing_toolchain_and_codecs(self):
        meta = metadata()
        for change in [lambda m: m['toolchain'].update(host='Mac'),
                       lambda m: m['toolchain'].update(cpp_runtime='libstdc++'),
                       lambda m: m['toolchain'].update(compiler=''),
                       lambda m: m['codecs'].update(video=[]),
                       lambda m: m['codecs'].update(audio=['opus', 'opus'])]:
            candidate = copy.deepcopy(meta)
            change(candidate)
            with self.assertRaises(ValueError):
                artifacts.validate_metadata(candidate)

    def test_invalid_json_and_duplicate_keys(self):
        (self.root / artifacts.MANIFEST).write_text('{"metadata":{},"metadata":{}}')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            artifacts.validate(self.root)

    def test_unsafe_paths(self):
        for path in ['../escape', '/absolute', 'C:/drive', 'a//b', 'a/../b', 'a\\b', './x', '.', '']:
            with self.subTest(path=path), self.assertRaises(ValueError):
                artifacts.relative_path(path)

    def test_symlink_is_rejected(self):
        (self.root / 'external').symlink_to(self.root.parent, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            artifacts.validate(self.root)

    def test_extra_library_or_java_payload_is_rejected(self):
        for name in ['libcrypto.a', 'helper.jar', 'helper.aar', 'duplicate.so']:
            path = self.root / name
            path.write_text('unwanted binary')
            seal(self.root)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'exactly one'):
                artifacts.validate(self.root)
            path.unlink()

    def test_source_lock_and_patch_integrity(self):
        lock_path = self.root / 'Build/source-lock.json'
        original = json.loads(lock_path.read_text())
        for change in [lambda d: d.update(webrtc_revision='wrong'), lambda d: d.update(dependencies={}),
                       lambda d: d.update(dependencies={'foo': 'main'}), lambda d: d.update(patches=['../patch'])]:
            lock = copy.deepcopy(original)
            change(lock)
            lock_path.write_text(json.dumps(lock))
            seal(self.root)
            with self.assertRaises(ValueError):
                artifacts.validate(self.root)
        patch = self.root / 'Build/patches/compat.patch'
        patch.parent.mkdir()
        patch.write_text('test patch')
        original['patches'] = ['Build/patches/compat.patch']
        lock_path.write_text(json.dumps(original))
        seal(self.root)
        artifacts.validate(self.root)
        patch.write_text('tampered patch')
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            artifacts.validate(self.root)

    def test_wrong_architecture_and_mixed_archive(self):
        path = self.root / 'Lib/libwebrtc.a'
        for data in [archive(('bad.o/', elf(62))), archive(('good.o/', elf()), ('bad.o/', elf(62))),
                     archive(('wrong.o/', macho()))]:
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                artifacts.validate_archive(path, 'Android')

    def test_ios_simulator_macos_and_fat_object_rejected(self):
        path = self.root / 'Lib/libwebrtc.a'
        for data in [macho(7), macho(1), macho(cpu=0x1000007), b'\xca\xfe\xba\xbe' + bytes(80)]:
            path.write_bytes(archive(('wrong.o/', data)))
            with self.assertRaises(ValueError):
                artifacts.validate_archive(path, 'IOS')

    def test_malformed_archives_rejected(self):
        path = self.root / 'Lib/libwebrtc.a'
        good = archive(('probe.o/', elf()))
        for data in [b'!<thin>\n', b'!<arch>\n', good[:-1], good[:35],
                     archive(('lto.o/', b'BC\xc0\xde' + bytes(64))), archive(('empty.o/', b''))]:
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                artifacts.validate_archive(path, 'Android')

    def test_gnu_and_bsd_archive_indices_and_long_names(self):
        path = self.root / 'Lib/libwebrtc.a'
        path.write_bytes(archive(('/', b'index'), ('//', b'a-very-long-name.o/\n'), ('/0', elf())))
        self.assertEqual(1, artifacts.validate_archive(path, 'Android'))
        index_name = b'__.SYMDEF SORTED\0'
        object_name = b'long-name.o\0'
        path.write_bytes(archive((f'#1/{len(index_name)}', index_name + b'index'),
                                 (f'#1/{len(object_name)}', object_name + macho())))
        self.assertEqual(1, artifacts.validate_archive(path, 'IOS'))

    def test_staging_profiles_select_artifacts_independently_of_provider(self):
        ios = self.root.parent / 'ios'
        make_payload(ios, 'IOS')
        seal(ios, 'IOS')
        for profile in ['full', 'direct-only', 'janus-only', 'data-only']:
            output = self.root.parent / profile
            profiles.stage(profile, output, [self.root, ios])
            plugin = output / 'Plugins/SimurghRTC'
            for platform in ['Android', 'IOS']:
                artifacts.validate(plugin / 'ThirdParty/WebRTC' / platform / 'arm64')
            if profile == 'direct-only':
                self.assertFalse((plugin / 'Source/SimurghRTCJanus').exists())

    def test_staging_rejects_duplicate_server_or_corrupt_inputs_before_writing(self):
        output = self.root.parent / 'stage'
        for profile, inputs in [('full', [self.root, self.root]), ('server', [self.root])]:
            with self.assertRaises(ValueError):
                profiles.stage(profile, output, inputs)
            self.assertFalse(output.exists())
        (self.root / 'Include/rtc_base/thread.h').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            profiles.stage('full', output, [self.root])
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
