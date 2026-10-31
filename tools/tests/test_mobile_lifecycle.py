"""Reject false deadline/process-death evidence and keep OS actions scoped to the sample."""
import copy
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from mobile_lifecycle import validate_deadline,validate_termination,android_lock_state,terminate_client,resume_after_unlock
from run_mobile_data_example_probe import MobileClient

class MobileLifecycleTests(unittest.TestCase):
    def deadline(self,expire):
        return dict(send_deadline=dict(expire=expire,phase=3,admitted=2,received=0 if expire else 2,
            settled_after_seconds=30.1 if expire else 4,quiet_seconds=3.1,peak_buffered_bytes=64),
            connection_errors=[8] if expire else [],rounds_completed=1,max_tick_gap_seconds=.1,heartbeats=100)
    def test_deadline_requires_full_wait_and_structured_timeout(self):
        r=self.deadline(True);validate_deadline(r,'deadline-expire')
        for key,value in [('settled_after_seconds',29.9),('quiet_seconds',0),('received',1),('phase',1),('peak_buffered_bytes',0)]:
            bad=copy.deepcopy(r);bad['send_deadline'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):validate_deadline(bad,'deadline-expire')
        for key,value in [('connection_errors',[11]),('max_tick_gap_seconds',3),('heartbeats',2)]:
            with self.subTest(key=key),self.assertRaises(ValueError):validate_deadline(dict(r,**{key:value}),'deadline-expire')
    def test_release_control_must_deliver_both_messages_before_deadline(self):
        r=self.deadline(False);validate_deadline(r,'deadline-release')
        for key,value in [('received',0),('settled_after_seconds',30.1),('settled_after_seconds',0)]:
            bad=copy.deepcopy(r);bad['send_deadline'][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):validate_deadline(bad,'deadline-release')
    def test_killed_process_cannot_claim_graceful_cleanup_or_reuse_session(self):
        r=dict(scenario='terminate-connected',client=dict(pid=43,session_id='new'),termination=dict(pid=42,signal='SIGKILL',
            process_absent=True,graceful_client_cleanup_claimed=False,final_report_absent=True,
            before=dict(state=2,transports_created=1,sent=2,received=2,exact_payloads=True,session_id='old'),
            server_after_death=dict(created=1,closed=1,expired=1,live_peers=0)))
        validate_termination(r)
        for key,value in [('signal','SIGTERM'),('process_absent',False),('graceful_client_cleanup_claimed',True),('final_report_absent',False)]:
            bad=copy.deepcopy(r);bad['termination'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):validate_termination(bad)
        bad=copy.deepcopy(r);bad['client']['pid']=42
        with self.assertRaises(ValueError):validate_termination(bad)
        bad=copy.deepcopy(r);bad['termination']['server_after_death']['expired']=0
        with self.assertRaises(ValueError):validate_termination(bad)
    def test_unlock_waits_for_async_resume_without_relaunching(self):
        from unittest.mock import Mock
        c=Mock();c.report.side_effect=[dict(in_background=True),None,dict(in_background=False)]
        with patch('mobile_lifecycle.time.sleep'):
            resume_after_unlock(c,'run',Path('evidence'))
        c.foreground.assert_not_called()
        self.assertEqual(c.report.call_count,3)
    def test_unlock_does_not_relaunch_a_probe_that_already_finished(self):
        from unittest.mock import Mock
        c=Mock();c.report.side_effect=[dict(in_background=True),dict(passed=True)]
        resume_after_unlock(c,'run',Path('evidence'));c.foreground.assert_not_called()
    def test_unlock_foregrounds_only_if_app_stays_in_background(self):
        from unittest.mock import Mock
        c=Mock();c.report.return_value=None
        with patch('mobile_lifecycle.time.monotonic',side_effect=[0,0,6]),patch('mobile_lifecycle.time.sleep'):
            resume_after_unlock(c,'run',Path('evidence'))
        c.foreground.assert_called_once_with()
    def test_failed_ui_action_is_never_recorded_as_verified(self):
        import tempfile,json,uuid
        from ack_mobile_ui import acknowledge
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'ui-request.json'
            path.write_text(json.dumps(dict(device=str(uuid.uuid4()),nonce=str(uuid.uuid4()),action='lock')))
            reply=acknowledge(path,'Host locked; action not performed.',failed=True)
            self.assertFalse(reply['verified'])
            self.assertEqual(json.loads(path.with_name('ui-response.json').read_text()),reply)
    def test_android_lock_requires_real_os_power_and_keyguard_state(self):
        c=MobileClient.__new__(MobileClient);c.adb=['adb','-s','owned']
        c.command=lambda args:subprocess.CompletedProcess(args,0,'mWakefulness=Asleep\n' if args[-1]=='power' else 'KeyguardServiceDelegate\n showing=true\n','')
        self.assertEqual(android_lock_state(c),dict(wakefulness='Asleep',keyguard_showing=True))
        c.command=lambda args:subprocess.CompletedProcess(args,0,'unknown','')
        with self.assertRaises(ValueError):android_lock_state(c)
    def test_android_kill_uses_only_the_confirmed_sample_uid_and_pid(self):
        c=MobileClient.__new__(MobileClient);c.simulator=False;c.pid=42;c.app_id='org.example.probe';c.adb=['adb','-s','owned']
        calls=[];c.assert_same_process=lambda:calls.append(['verified'])
        c.command=lambda args,**kwargs:(calls.append(args) or subprocess.CompletedProcess(args,0,'',''))
        result=terminate_client(c)
        self.assertEqual(calls[0],['verified'])
        self.assertIn(c.adb+['shell','run-as',c.app_id,'kill','-9','42'],calls)
        self.assertTrue(result['process_absent']);self.assertFalse(result['graceful_client_cleanup_claimed'])
    def test_simulator_kill_requires_observed_process_disappearance(self):
        c=MobileClient.__new__(MobileClient);c.simulator=True;c.pid=42;c.assert_same_process=lambda:None
        with patch('mobile_lifecycle.os.kill') as kill,patch('mobile_lifecycle.process_alive',return_value=False):
            terminate_client(c);self.assertEqual(kill.call_args.args[0],42)

if __name__=='__main__':unittest.main()
