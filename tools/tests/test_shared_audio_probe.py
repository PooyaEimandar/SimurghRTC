import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_shared_audio_probe import validate

class SharedAudioReportTests(unittest.TestCase):
    def report(self,server=False):
        r=dict(schema=1,run_id='run',pid=123,server=server,server_build=server,editor_build=False,with_janus=False,
               loopback_only=True,audio_hardware_opened=False,passed=True,error='',phases=4,checks=0 if server else 4,
               pending_cleanup=0,client_native_closed=0 if server else 1,client_remote_closed=0 if server else 1,
               artifact_manifest_sha256='sha',engine_version='5.8.3-test',quiet_ticks=8)
        r['measurements']=[dict(phase=i,samples=72000,correlation=0 if server and i==1 else .99,rms=0 if server and i==1 else .176) for i in range(4)]
        if server:r.update(server_live_peers=0,server_closed=1)
        return r
    def check(self,r,server=False):validate(r,'run',123,'sha',server,False)
    def test_rejects_missing_report(self):
        with self.assertRaises(ValueError):self.check(None)
    def test_accepts_complete_two_way_measurements(self):
        self.check(self.report());self.check(self.report(True),True)
    def test_rejects_wrong_package_owner_or_cleanup(self):
        for key,value in [('run_id','old'),('pid',124),('artifact_manifest_sha256','stale'),('editor_build',True),('server_build',True),('with_janus',True),('pending_cleanup',1),('client_native_closed',0),('client_remote_closed',0),('quiet_ticks',0),('checks',3)]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=self.report();r[key]=value;self.check(r)
    def test_rejects_missing_phase_or_false_signal(self):
        cases=[]
        for key,value in [('samples',0),('correlation',.5),('correlation',float('nan')),('correlation',True),('rms',0),('rms',float('inf')),('phase',2)]:
            r=self.report();r['measurements'][0][key]=value;cases.append(r)
        r=self.report();r['measurements'].pop();cases.append(r)
        for r in cases:
            with self.assertRaises(ValueError):self.check(r)
    def test_rejects_muted_audio_or_live_server_peer(self):
        r=self.report(True);r['measurements'][1]['rms']=.1
        with self.assertRaises(ValueError):self.check(r,True)
        r=self.report(True);r['server_live_peers']=1
        with self.assertRaises(ValueError):self.check(r,True)
