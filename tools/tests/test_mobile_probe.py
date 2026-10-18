"""Reject misleading lifecycle evidence before it can count as mobile acceptance."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).parents[1]))
import prepare_mobile_probe
import ios_simulator_support
from run_mobile_probe import android_main_pid, android_package_sizes, ios_simulator, ios_simulator_runtime, legacy_ios_device, physical_ios_device, read_report, read_simulator_report, simulator_launch_pid, validate_report
from test_mobile_artifacts import archive, macho, make_payload, seal


class MobileEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(schema=1, run_id='test-run', platform='Android', engine_version='5.8.3-test',
                           pid=123, editor_build=False, server_build=False, arm64_build=True, simulator_build=False, with_janus=False,
                           artifact_manifest_sha256='a' * 64, network_transport_tested=False,
                           hardware_capture_tested=False, completed_cycles=3, factories_created=9, peers_created=9,
                           explicit_teardowns=6, destructor_teardowns=3, overlapping_owners_checked=True,
                           duplicate_calls_checked=True, readiness_cleared_after_stop=True,
                           elapsed_seconds=0.1, passed=True, error='')

    def validate(self, report):
        validate_report(report, 'test-run', 'Android', 'a' * 64, False)

    def test_complete_device_evidence(self):
        self.validate(self.report)
        ios = dict(self.report, platform='IOS', with_janus=True)
        validate_report(ios, 'test-run', 'IOS', 'a' * 64, True)

    def test_rejects_wrong_artifact_profile_platform_and_editor(self):
        for key, value in [('run_id', 'old'), ('artifact_manifest_sha256', 'b' * 64), ('with_janus', True),
                           ('platform', 'Mac'), ('editor_build', True), ('server_build', True),
                           ('arm64_build', False), ('engine_version', '5.9.0-test')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(dict(self.report, **{key: value}))

    def test_rejects_missing_teardown_and_false_success(self):
        for key in ['completed_cycles', 'factories_created', 'peers_created', 'explicit_teardowns',
                    'destructor_teardowns', 'overlapping_owners_checked', 'duplicate_calls_checked',
                    'readiness_cleared_after_stop', 'passed']:
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(dict(self.report, **{key: 0}))

    def test_strict_types_and_finite_time(self):
        for key, value in [('schema', True), ('pid', True), ('pid', 0), ('with_janus', 0),
                           ('engine_version', None), ('elapsed_seconds', float('nan')),
                           ('elapsed_seconds', float('inf')), ('elapsed_seconds', -1),
                           ('elapsed_seconds', 121), ('elapsed_seconds', True)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.validate(dict(self.report, **{key: value}))

    def test_only_one_correlated_log_report(self):
        prefix = 'LogSimurghRTCClientProbe: Display: SIMURGHRTC_CLIENT_REPORT '
        valid = prefix + json.dumps(self.report)
        stale = prefix + json.dumps(dict(self.report, run_id='stale'))
        partial = prefix + '{"run_id":'
        self.assertIsNone(read_report(stale + '\n' + partial, 'test-run'))
        self.assertEqual(self.report, read_report(stale + '\n' + valid + '\n' + partial, 'test-run'))
        self.assertIsNone(read_report(prefix + '[]', 'test-run'))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            read_report(valid + '\n' + valid, 'test-run')

    def test_coredevice_requires_physical_ios(self):
        device = {'identifier': 'phone', 'hardwareProperties': {'reality': 'physical', 'platform': 'iOS'}}
        document = {'result': {'devices': [device]}}
        self.assertEqual(device, physical_ios_device(document, 'phone'))
        for key, value in [('reality', 'simulated'), ('platform', 'macOS')]:
            bad = copy.deepcopy(document)
            bad['result']['devices'][0]['hardwareProperties'][key] = value
            with self.assertRaises(ValueError):
                physical_ios_device(bad, 'phone')
        with self.assertRaises(ValueError):
            physical_ios_device(document, 'another-device')

    def test_simulator_is_explicit_and_never_counts_as_device(self):
        report = dict(self.report, platform='IOS', simulator_build=True)
        validate_report(report, 'test-run', 'IOS', 'a' * 64, False, simulator=True)
        with self.assertRaisesRegex(ValueError, 'simulator_build'):
            validate_report(report, 'test-run', 'IOS', 'a' * 64, False)
        device = {'udid': 'sim', 'isAvailable': True}
        inventory = {'devices': {'com.apple.CoreSimulator.SimRuntime.iOS-27-0': [device]}}
        self.assertEqual('sim', ios_simulator(inventory, 'sim')['udid'])
        device['isAvailable'] = False
        with self.assertRaises(ValueError):
            ios_simulator(inventory, 'sim')

    def test_simulator_report_is_bound_to_launch_pid(self):
        self.assertEqual(123, simulator_launch_pid('org.example.probe: 123\n', 'org.example.probe'))
        for log in ['org.other.probe: 123\n', 'org.example.probe: 0\n',
                    'org.example.probe: 123\norg.example.probe: 456\n']:
            with self.assertRaises(ValueError):
                simulator_launch_pid(log, 'org.example.probe')

    def test_simulator_runtime_must_support_arm64(self):
        runtime = dict(identifier='ios', name='iOS test', version='27.0', buildversion='test',
                       platform='iOS', isAvailable=True, supportedArchitectures=['arm64'])
        self.assertEqual('27.0', ios_simulator_runtime({'runtimes': [runtime]}, 'ios')['version'])
        for changes in [dict(supportedArchitectures=['x86_64']), dict(platform='tvOS'), dict(isAvailable=False)]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                ios_simulator_runtime({'runtimes': [dict(runtime, **changes)]}, 'ios')
        with self.assertRaises(ValueError):
            ios_simulator_runtime({'runtimes': []}, 'ios')

    def test_saved_simulator_report_ignores_stale_and_partial_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'client-probe-stale.json').write_text(json.dumps(self.report))
            self.assertIsNone(read_simulator_report(root, 'test-run'))
            current = root / 'client-probe-test-run.json'
            current.write_text('{')
            self.assertIsNone(read_simulator_report(root, 'test-run'))
            current.write_text(json.dumps(self.report))
            self.assertEqual(self.report, read_simulator_report(root, 'test-run'))
            (root / 'duplicate').mkdir()
            (root / 'duplicate' / current.name).write_text(current.read_text())
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                read_simulator_report(root, 'test-run')

    def test_legacy_device_requires_usb_model_and_arm64(self):
        self.assertEqual('16.7.16', legacy_ios_device('phone\n', 'phone', 'iPhone10,6', '16.7.16', 'arm64')['os_version'])
        for values in [('other\n', 'phone', 'iPhone10,6', '16.7', 'arm64'),
                       ('phone\n', 'phone', 'Simulator', '16.7', 'arm64'),
                       ('phone\n', 'phone', 'iPhone10,6', '16.7', 'x86_64')]:
            with self.assertRaises(ValueError):
                legacy_ios_device(*values)

    def test_apk_sizes_reject_other_abis_and_do_not_claim_shipping_delta(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / 'fixture.apk'
            with zipfile.ZipFile(app, 'w') as package:
                package.writestr('lib/arm64-v8a/libUnreal.so', b'test-only')
            sizes = android_package_sizes(app)
            self.assertEqual(app.stat().st_size, sizes['apk_bytes'])
            self.assertFalse(sizes['shipping_baseline'])
            self.assertFalse(sizes['rtc_size_delta_measured'])
            with zipfile.ZipFile(app, 'a') as package:
                package.writestr('lib/x86_64/libUnreal.so', b'test-only')
            with self.assertRaisesRegex(ValueError, 'ARM64-only'):
                android_package_sizes(app)

    def test_android_main_process_allows_unreal_crash_handler_child(self):
        rows = 'PID PPID NAME\n101 50 org.example.app\n102 101 org.example.app\n'
        self.assertEqual(101, android_main_pid(rows, 'org.example.app'))
        self.assertEqual(101, android_main_pid(rows.replace('102 101', '102 1'), 'org.example.app'))
        for bad in [rows.replace('102 101', '102 50'), rows.replace('102 101 org.example.app', '102 101 other.app'),
                    'PID PPID NAME\n', rows.replace('101 50', '101 102')]:
            with self.assertRaises(ValueError):
                android_main_pid(bad, 'org.example.app')


class MobilePreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.artifact = self.root / 'artifact'
        make_payload(self.artifact)
        seal(self.artifact)

    def support(self, platform=7):
        root = self.root / 'support'
        root.mkdir()
        (root / 'libnghttp2.a').write_bytes(archive(('http2.o/', macho(platform=platform))))
        for name in ['LICENSE.txt', 'build.json', 'source-lock.json']:
            (root / name).write_text('Synthetic test fixture')
        files = {p.name: ios_simulator_support.artifacts.digest(p) for p in root.iterdir()}
        (root / 'support.json').write_text(json.dumps(dict(schema=1, platform='IOSSimulator', architecture='arm64',
                                                         archive_sha256=files['libnghttp2.a'], files=files)))
        return root

    def test_simulator_support_rejects_tampering_and_extra_files(self):
        support = self.support()
        self.assertIn('archive_sha256', ios_simulator_support.validate(support))
        (support / 'extra.a').write_bytes(b'extra')
        with self.assertRaisesRegex(ValueError, 'payload'):
            ios_simulator_support.validate(support)
        (support / 'extra.a').unlink()
        (support / 'libnghttp2.a').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed'):
            ios_simulator_support.validate(support)

    def test_simulator_support_rejects_device_objects(self):
        with self.assertRaises(ValueError):
            ios_simulator_support.validate(self.support(platform=2))

    def test_simulator_support_stages_only_with_simulator_artifact(self):
        support = self.support()
        with self.assertRaisesRegex(ValueError, 'IOSSimulator artifact'):
            prepare_mobile_probe.stage(self.root / 'bad-support', 'direct-only', [self.artifact],
                                       'org.example.probe', simulator_support=support)
        artifact = self.root / 'simulator'
        make_payload(artifact, 'IOSSimulator')
        seal(artifact, 'IOSSimulator')
        output = self.root / 'simulator-project'
        record = prepare_mobile_probe.stage(output, 'direct-only', [artifact], 'org.example.probe',
                                           simulator_support=support)
        self.assertEqual(record['simulator_support'], ios_simulator_support.validate(output / 'Build/IOSSimulator'))
        self.assertIn('MaxFeatureLevel=SM5', (output / 'Platforms/IOS/Config/DataDrivenPlatformInfo.ini').read_text())
        self.assertTrue((output / 'Config/IOS/IOSEngine.ini').is_file())

    def test_stage_keeps_identity_and_platform_helpers(self):
        destination = self.root / 'direct'
        record = prepare_mobile_probe.stage(destination, 'direct-only', [self.artifact], 'org.example.probe')
        self.assertFalse(record['with_janus'])
        self.assertEqual('unverified', record['runtime_validation'])
        self.assertEqual(record, json.loads((destination / 'mobile-probe.json').read_text()))
        project = json.loads((destination / 'ContractHost.uproject').read_text())
        helper = next(p for p in project['Plugins'] if p['Name'] == 'AndroidPermission')
        self.assertTrue(helper['Enabled'])
        self.assertEqual(['Android'], helper['PlatformAllowList'])
        self.assertFalse((destination / 'Plugins/SimurghRTC/Source/SimurghRTCJanus').exists())
        self.assertFalse((destination / 'Platforms/IOS').exists())
        config = (destination / 'Config/DefaultEngine.ini').read_text()
        self.assertIn('[/Script/MacTargetPlatform.XcodeProjectSettings]\nBundleIdentifier=org.example.probe', config)
        self.assertIn('bUseSceneBasedLifecycle=True', config)
        with self.assertRaises(ValueError):
            prepare_mobile_probe.stage(destination, 'full', [self.artifact], 'org.example.probe')

    def test_signing_is_explicit_and_inputs_are_validated(self):
        for profile, artifacts, app_id, team in [('server', [self.artifact], 'org.example.probe', ''),
                                                ('full', [], 'org.example.probe', ''),
                                                ('full', [self.artifact], 'org.example.probe\nInjected=True', ''),
                                                ('full', [self.artifact], 'org.example.probe', 'invalid')]:
            with self.assertRaises(ValueError):
                prepare_mobile_probe.stage(self.root / 'bad', profile, artifacts, app_id, team)
        output = self.root / 'signed'
        record = prepare_mobile_probe.stage(output, 'full', [self.artifact], 'org.example.probe', 'TESTTEAM01')
        self.assertTrue(record['with_janus'])
        self.assertTrue(record['ios_signing_configured'])
        self.assertIn('CodeSigningTeam=TESTTEAM01', (output / 'Config/DefaultEngine.ini').read_text())

    def test_device_and_simulator_artifacts_stay_separate(self):
        inputs = []
        for platform in ('IOS', 'IOSSimulator'):
            path = self.root / platform
            make_payload(path, platform)
            seal(path, platform)
            inputs.append(path)
        output = self.root / 'ios-probes'
        record = prepare_mobile_probe.stage(output, 'direct-only', inputs, 'org.example.simprobe')
        self.assertEqual({'IOS', 'IOSSimulator'}, set(record['artifacts']))
        self.assertFalse(record['ios_signing_configured'])
        for platform in record['artifacts']:
            self.assertTrue((output / 'Plugins/SimurghRTC/ThirdParty/WebRTC' / platform / 'arm64/Lib/libwebrtc.a').is_file())


if __name__ == '__main__':
    unittest.main()
