"""Device link evidence must never silently become runtime acceptance."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_native_abi_probe import run_probe
from test_native_artifacts import make_native_payload, seal_native


class NativeLinkOnlyTests(unittest.TestCase):
    def test_ios_link_does_not_sign_launch_or_claim_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact, output = root / 'artifact', root / 'report'
            make_native_payload(artifact, 'IOS')
            lock = json.loads((artifact / 'Build/source-lock.json').read_text())
            build = json.loads((artifact / 'Build/build.json').read_text())
            build['exports'] = (Path(__file__).resolve().parents[2] / 'Config/NativeBackend/exports.txt').read_text().split()
            (artifact / 'Build/build.json').write_text(json.dumps(build))
            seal_native(artifact)
            calls = []

            def command(args, **kwargs):
                calls.append(args)
                text = ''
                if args[0] == 'xcodebuild': text = lock['mac']['xcode']
                elif '--show-sdk-version' in args: text = lock['platforms']['IOS']['sdk']
                elif '--show-sdk-path' in args: text = '/sdk'
                elif 'lipo' in args: text = 'arm64'
                elif 'nm' in args: text = '\n'.join('000 T ' + symbol for symbol in build['exports'])
                elif 'vtool' in args: text = 'platform IOS\n'
                elif '-o' in args: Path(args[args.index('-o') + 1]).write_bytes(b'fixture')
                else: self.fail('Unexpected signing or runtime command: ' + str(args))
                return subprocess.CompletedProcess(args, 0, text, '')

            with patch('run_native_abi_probe.subprocess.run', side_effect=command), \
                 patch('run_native_abi_probe.platform.system', return_value='Darwin'), \
                 patch('run_native_abi_probe.platform.machine', return_value='arm64'):
                result = run_probe(artifact, output, link_only=True)
            self.assertTrue(result['passed'], result.get('error'))
            self.assertEqual('not_run', result['runtime_validation'])
            self.assertNotIn('peers_created', result)
            self.assertEqual(result, json.loads((output / 'result.json').read_text()))
            self.assertFalse((output / 'runtime.log').exists())
            self.assertTrue(any('arm64-apple-ios15.0' in args for args in calls))

    def test_device_runtime_and_simulator_link_only_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for target, link_only in [('IOS', False), ('IOSSimulator', True)]:
                artifact = root / target
                make_native_payload(artifact, target)
                with self.assertRaises(ValueError):
                    run_probe(artifact, root / 'output', link_only=link_only)
