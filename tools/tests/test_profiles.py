"""Packaging policy checks; no Unreal installation is required."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('prepare_profile', Path(__file__).parents[1] / 'prepare_profile.py')
profiles = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profiles)


class ProfileTests(unittest.TestCase):
    def test_default_matches_policy(self):
        actual = tuple(json.loads((profiles.PLUGIN / name).read_text())
                       for name in ['SimurghRTC.uplugin', 'SimurghRTC.profile.json'])
        self.assertEqual(profiles.documents('full'), actual)

    def test_all_profiles_stage_portable_hosts_and_exact_modules(self):
        policy = json.loads(profiles.POLICY.read_text())
        with tempfile.TemporaryDirectory() as temporary:
            for name, expected in policy['profiles'].items():
                with self.subTest(profile=name):
                    output = Path(temporary) / name
                    profiles.stage(name, output)
                    plugin = output / 'Plugins' / 'SimurghRTC'
                    descriptor = json.loads((plugin / 'SimurghRTC.uplugin').read_text())
                    manifest = json.loads((plugin / 'SimurghRTC.profile.json').read_text())
                    self.assertEqual(set(expected), {p.name for p in (plugin / 'Source').iterdir()})
                    self.assertEqual(expected, [m['Name'] for m in descriptor['Modules']])
                    self.assertEqual(expected, manifest['modules'])
                    self.assertEqual('shared-direct-data' if manifest['with_direct'] else 'internal-sdp-ice', manifest['native_transport'])
                    self.assertEqual('5.8.0', descriptor['EngineVersion'])
                    self.assertEqual('5.8.3', manifest['engine_version'])
                    host = json.loads((output / 'ContractHost.uproject').read_text())
                    self.assertNotIn('AdditionalPluginDirectories', host)
                    self.assertTrue((output / 'Source' / 'ContractHostServer.Target.cs').is_file())
                    self.assertTrue((plugin / 'LICENSE').is_file())
                    blueprint = Path('Content/Examples/BP_RtcLifecycle.uasset')
                    self.assertEqual((profiles.ROOT / 'Samples/ContractHost' / blueprint).read_bytes(),
                                     (output / blueprint).read_bytes())
                    janus = Path('Content/Examples/Janus/BP_JanusProfile.uasset')
                    if 'SimurghRTCJanusExtensions' in expected:
                        self.assertEqual((profiles.ROOT / 'Samples/ContractHost' / janus).read_bytes(),
                                         (output / janus).read_bytes())
                    else:
                        self.assertFalse((output / janus.parent).exists())
                    self.assertEqual('SimurghRTCJanusExtensions' in expected, manifest['with_janus_extensions'])
                    for module in expected:
                        build = (plugin / 'Source' / module / f'{module}.Build.cs').read_text()
                        spec = policy['modules'][module]
                        dependencies = (spec['dependencies'] + spec.get('private_dependencies', [])
                                        + spec.get('desktop_third_party_dependencies', []))
                        for dep in dependencies:
                            self.assertIn(f'"{dep}"', build)

    def test_optional_reflected_modules_are_physically_absent(self):
        with tempfile.TemporaryDirectory() as temporary:
            for name in ['direct-only', 'data-only', 'server']:
                output = Path(temporary) / name
                profiles.stage(name, output)
                source = output / 'Plugins' / 'SimurghRTC' / 'Source'
                self.assertFalse((source / 'SimurghRTCJanusExtensions').exists())
                if name != 'data-only':
                    self.assertFalse((source / 'SimurghRTCJanus').exists())
                if name != 'direct-only':
                    self.assertFalse((source / 'SimurghRTCMedia').exists())

    def test_server_does_not_enable_client_modules_or_mobile(self):
        descriptor, manifest = profiles.documents('server')
        self.assertFalse(manifest['with_janus'])
        self.assertFalse(manifest['with_media'])
        self.assertTrue(manifest['with_server'])
        self.assertNotIn('Android', descriptor['SupportedTargetPlatforms'])
        self.assertNotIn('IOS', descriptor['SupportedTargetPlatforms'])
        full, _ = profiles.documents('full')
        modules = {m['Name']: m for m in full['Modules']}
        for name in ['SimurghRTCJanus', 'SimurghRTCJanusExtensions', 'SimurghRTCMedia']:
            self.assertIn('Server', modules[name]['TargetDenyList'])

    def test_refuses_overwriting_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            marker = output / 'keep.txt'
            marker.write_text('existing work')
            with self.assertRaises(ValueError):
                profiles.stage('full', output)
            self.assertEqual('existing work', marker.read_text())
            self.assertEqual([marker], list(output.iterdir()))

    def test_refuses_plugin_tree_and_ancestors(self):
        for destination in [profiles.ROOT, profiles.ROOT.parent, profiles.PLUGIN / 'staged',
                            profiles.ROOT / 'Samples' / 'ContractHost' / 'Source' / 'staged']:
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                profiles.stage('full', destination)


if __name__ == '__main__':
    unittest.main()
