import copy
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_direct_probe import validate_client, validate_server

SESSION='11111111-1111-4111-8111-111111111111'
def report():
    return dict(schema=1,run_id='run',pid=42,server_build=False,editor_build=False,arm64_build=True,platform='Mac',with_janus=False,
                artifact_manifest_sha256='hash',native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',loopback_only=True,
                network_media_tested=False,application_messages_tested=False,callbacks_on_game_thread=True,passed=True,error='',
                scenario='connect',expected_error='',requested_pairs=1,completed_pairs=1,owner_callback_suppressed=True,
                engine_version='5.8.3-0+UE5',elapsed_seconds=1.2,
                pairs=[dict(index=0,session_id=SESSION,mode='data',early_ice=True,native_closed=True,remote_closed=True,requests=8,retries=0,
                            transport=dict(connection=2,ice=3,dtls=3,protocol=1,sctp_open=True,remote_end=True,queued_ice=0,
                                           applied_ice=2,stats_samples=4,stun_responses=5))])
class DirectProbeTests(unittest.TestCase):
    def validate(self,r,**kwargs):validate_client(r,'run',42,'hash',False,pairs=1,**kwargs)
    def test_connected_and_cleaned_up(self):self.validate(report())
    def test_rejects_wrong_process_artifact_and_false_success(self):
        for key,value in [('pid',43),('artifact_manifest_sha256','old'),('server_build',True),('editor_build',True),('passed',False),
                          ('completed_pairs',0),('elapsed_seconds',float('nan')),('owner_callback_suppressed',False)]:
            with self.subTest(key=key):
                r=report();r[key]=value
                with self.assertRaises(ValueError):self.validate(r)
    def test_rejects_incomplete_transport_and_cleanup(self):
        for key,value in [('dtls',2),('ice',4),('sctp_open',False),('remote_end',False),('queued_ice',1),('stats_samples',0)]:
            with self.subTest(key=key):
                r=report();r['pairs'][0]['transport'][key]=value
                with self.assertRaises(ValueError):self.validate(r)
        for key in ['native_closed','remote_closed']:
            r=report();r['pairs'][0][key]=False
            with self.assertRaises(ValueError):self.validate(r)
    def test_expected_failure_is_explicit_and_still_requires_cleanup(self):
        r=report();r['expected_error']='Unauthorized';r['pairs']=[dict(expected_error='Unauthorized',native_closed=True,retries=0)]
        self.validate(r,expected_error='Unauthorized')
        r['pairs'][0]['expected_error']='TimedOut'
        with self.assertRaises(ValueError):self.validate(r,expected_error='Unauthorized')
    def test_cancellation_has_remote_and_native_confirmation(self):
        r=report();r['scenario']='owner-loss';r['pairs']=[dict(session_id=SESSION,cancelled=True,native_closed=True,remote_closed=True)]
        self.validate(r,scenario='owner-loss')
        del r['pairs'][0]['cancelled']
        with self.assertRaises(ValueError):self.validate(r,scenario='owner-loss')
    def test_server_identity_and_complete_session_inventory(self):
        r=report();r.update(server_build=True,running=False,live_peers=0,teardown_complete=True,
                            created=1,connected=1,closed=1,expired=0,replays=2,rejected=2,early_ice=1,
                            connected_session_ids=[SESSION],closed_session_ids=[SESSION],last_transport=r['pairs'][0]['transport'])
        validate_server(r,'run',42,'hash')
        network=copy.deepcopy(r);network['loopback_only']=False
        validate_server(network,'run',42,'hash',loopback=False)
        with self.assertRaises(ValueError):validate_server(network,'run',42,'hash')
        for key,value in [('server_build',False),('pid',43),('live_peers',1),('closed_session_ids',[]),('teardown_complete',False)]:
            bad=copy.deepcopy(r);bad[key]=value
            with self.assertRaises(ValueError):validate_server(bad,'run',42,'hash')
