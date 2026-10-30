"""Mobile evidence must identify the real platform, route, package and cleanup."""
from pathlib import Path
import subprocess
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_data_example_probe import validate_example
from run_mobile_data_example_probe import MobileClient
import test_data_example_probe as examples


class MobileDataExampleTests(unittest.TestCase):
    def validate(self, value, simulator):
        return validate_example(value, 'run', 42, 'digest', True, 'blueprint', 'messages',
                                platform='IOS' if simulator else 'Android', simulator=simulator, loopback=simulator)

    def test_mobile_identity_and_route_are_required(self):
        for simulator in (False, True):
            value = examples.DataExampleProbeTests().report()
            value.update(platform='IOS' if simulator else 'Android', simulator_build=simulator, loopback_only=simulator)
            self.validate(value, simulator)
            for key, wrong in [('platform', 'Mac'), ('simulator_build', not simulator), ('loopback_only', not simulator),
                               ('artifact_manifest_sha256', 'old'), ('native_closed', 0), ('objects_collected', False)]:
                with self.subTest(simulator=simulator, key=key), self.assertRaises(ValueError):
                    self.validate(dict(value, **{key: wrong}), simulator)

    def test_simulator_credentials_are_private_and_removed(self):
        with tempfile.TemporaryDirectory() as temp:
            c = MobileClient.__new__(MobileClient); c.simulator=True; c.pid=None; c.reverses=[]
            path=Path(temp)/'token';c.paths=[path]
            c.write(path,'secret-token')
            self.assertEqual(path.stat().st_mode & 0o777,0o600)
            self.assertEqual(path.read_text(),'secret-token')
            with self.assertRaises(FileExistsError):c.write(path,'replacement')
            c.cleanup();self.assertFalse(path.exists())

    def test_android_secret_never_enters_command_log(self):
        with tempfile.TemporaryDirectory() as temp:
            c=MobileClient.__new__(MobileClient);c.output=Path(temp)
            with patch('run_mobile_data_example_probe.subprocess.run',return_value=subprocess.CompletedProcess([],0,'secret-token','')) as call:
                c.command(['adb','shell','run-as','org.example.probe','tee','files/token'],secret='secret-token')
                self.assertEqual(call.call_args.kwargs['input'],'secret-token')
            self.assertNotIn('secret-token',(c.output/'commands.log').read_text())

    def test_android_cleanup_removes_only_its_reverse_ports(self):
        c=MobileClient.__new__(MobileClient);c.simulator=False;c.pid=42;c.adb=['adb','-s','device'];c.app_id='org.example.probe'
        c.reverses=['42000','42001'];c.paths=['/data/user/0/org.example.probe/files/run/token'];calls=[]
        c.command=lambda command: calls.append(command)
        c.cleanup()
        self.assertEqual(c.reverses,[])
        self.assertIn(['adb','-s','device','reverse','--remove','tcp:42000'],calls)
        self.assertFalse(any('--remove-all' in command for command in calls))
        self.assertIsNone(c.pid)

    def test_simulator_arguments_use_unreal_private_relative_paths(self):
        c=MobileClient.__new__(MobileClient);c.simulator=True;c.container=Path('/Users/example/simulator/app')
        self.assertEqual(c.argument_path(c.container/'Library/Caches/run/token'),'Caches/run/token')
        with self.assertRaises(ValueError):c.argument_path(Path('/tmp/unrelated'))
        c.simulator=False
        self.assertEqual(c.argument_path('/data/user/0/org.example.probe/files/token'),'/data/user/0/org.example.probe/files/token')

    def test_simulator_cleanup_accepts_exit_race_and_checks_process_disappearance(self):
        c=MobileClient.__new__(MobileClient);c.simulator=True;c.pid=42;c.reverses=[];c.device='sim';c.app_id='org.example.probe'
        calls=[]
        c.command=lambda args,**kwargs: calls.append((args,kwargs))
        with patch('run_mobile_data_example_probe.process_alive',side_effect=[True,False]):c.stop()
        self.assertIsNone(c.pid)
        self.assertEqual(calls[0][1],{'check':False})

    def test_android_reads_complete_private_report_when_log_is_truncated(self):
        with tempfile.TemporaryDirectory() as temp:
            c=MobileClient.__new__(MobileClient);c.simulator=False;c.pid=42;c.app_id='org.example.probe';c.adb=['adb','-s','device']
            run_id='00000000-0000-4000-8000-000000000001';value={'run_id':run_id,'passed':True}
            calls=[]
            def command(args,**kwargs):
                calls.append(args)
                return subprocess.CompletedProcess(args,0,json.dumps(value) if 'cat' in args else 'SIMURGHRTC_DATA_EXAMPLE_REPORT {"schema":','')
            c.command=command
            self.assertEqual(c.report(run_id,Path(temp)),value)
            self.assertIn('run-as',calls[0]);self.assertTrue(calls[0][-1].endswith('data-example-'+run_id+'.json'))

    def test_background_resume_keeps_the_existing_process(self):
        for simulator in (True,False):
            c=MobileClient.__new__(MobileClient);c.simulator=simulator;c.pid=42;c.device='device';c.app_id='org.example.probe';c.adb=['adb','-s','device']
            calls=[]
            def command(args,**kwargs):
                calls.append(args);return subprocess.CompletedProcess(args,0,'42' if 'pidof' in args else 'org.example.probe: 42','')
            c.command=command
            with patch('run_mobile_data_example_probe.process_alive',return_value=True):c.background();c.foreground()
            self.assertEqual(c.pid,42)
            self.assertFalse(any('force-stop' in args or '--terminate-running-process' in args for args in calls))
            if simulator:self.assertIn(['xcrun','simctl','launch','device','com.apple.Preferences'],calls)
            else:self.assertIn(['adb','-s','device','shell','input','keyevent','KEYCODE_HOME'],calls)


if __name__ == '__main__':unittest.main()
