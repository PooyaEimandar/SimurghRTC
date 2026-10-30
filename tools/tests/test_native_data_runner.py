"""The shared native runner must distinguish DataChannel evidence from lifecycle output."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_native_abi_probe import run_probe
from test_native_artifacts import make_native_payload, seal_native


class NativeDataRunnerTests(unittest.TestCase):
    def run_fixture(self, output_text, code=0):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);artifact=root/'artifact';make_native_payload(artifact,'Mac')
            lock=json.loads((artifact/'Build/source-lock.json').read_text())
            build=json.loads((artifact/'Build/build.json').read_text())
            build['exports']=(Path(__file__).resolve().parents[2]/'Config/NativeBackend/exports.txt').read_text().split()
            (artifact/'Build/build.json').write_text(json.dumps(build));seal_native(artifact)
            def command(args,**kwargs):
                text='';exit_code=0
                if args[0]=='xcodebuild':text=lock['mac']['xcode']
                elif '--show-sdk-version' in args:text=lock['platforms']['Mac']['sdk']
                elif '--show-sdk-path' in args:text='/sdk'
                elif 'lipo' in args:text='arm64'
                elif 'nm' in args:text='\n'.join('000 T '+symbol for symbol in build['exports'])
                elif 'vtool' in args:text='platform MACOS\n'
                elif '-o' in args:Path(args[args.index('-o')+1]).write_bytes(b'consumer')
                elif args[0]=='codesign':pass
                else:text=output_text;exit_code=code
                return subprocess.CompletedProcess(args,exit_code,text,'')
            with patch('run_native_abi_probe.subprocess.run',side_effect=command), \
                 patch('run_native_abi_probe.platform.system',return_value='Darwin'), \
                 patch('run_native_abi_probe.platform.machine',return_value='arm64'):
                return run_probe(artifact,root/'result',probe='data')

    def test_data_result_records_both_compiled_fixture_inputs(self):
        result=self.run_fixture('PASS: DataChannel UTF-8/binary round trips')
        self.assertTrue(result['passed'],result.get('error'))
        self.assertEqual(result['channel_modes'],6)
        self.assertEqual(result['negotiated_limits'],[32768,65536])
        self.assertEqual(set(result['fixture_inputs']),{'Fixtures/native/data_channels.c','Fixtures/native/signaling.c'})
        self.assertNotIn('codec_runs',result)

    def test_lifecycle_output_or_unsuccessful_exit_cannot_pass_data(self):
        for text,code in [('PASS: six peers, overlapping owners, ABI rejection, repeated VP8/Opus;',0),
                          ('PASS: DataChannel UTF-8/binary round trips',1)]:
            with self.subTest(text=text,code=code):self.assertFalse(self.run_fixture(text,code)['passed'])


if __name__=='__main__':unittest.main()
