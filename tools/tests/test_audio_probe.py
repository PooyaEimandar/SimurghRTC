"""Audio acceptance rejects codec-only, wrong-artifact and unresponsive-game-thread evidence."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_audio_probe import validate

class AudioProbeTests(unittest.TestCase):
    def report(self):
        return dict(schema=1,run_id='run',passed=True,error='',pairs=2,peers_closed=4,
                    loopback_only=True,audio_hardware_opened=False,editor_build=False,server_build=True,
                    artifact_manifest_sha256='artifact',native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',
                    engine_version='5.8.3-test',minimum_correlation=.999,maximum_muted_rms=.0001,seconds=14.5,
                    max_tick_gap_seconds=.03,decoded_samples=1000000,heartbeats=500)
    def test_complete_probe(self):
        validate(self.report(),'run',dict(manifest_sha256='artifact'),'server')
    def test_reject_missing_or_wrong_transport_evidence(self):
        for key,value in [('peers_closed',2),('pairs',0),('artifact_manifest_sha256','old'),('run_id','old'),
                          ('decoded_samples',0),('minimum_correlation',.89),('maximum_muted_rms',.1),
                          ('max_tick_gap_seconds',3),('heartbeats',1),('audio_hardware_opened',True),
                          ('minimum_correlation',float('nan')),('seconds',float('inf')),('server_build',1)]:
            report=copy.deepcopy(self.report());report[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate(report,'run',dict(manifest_sha256='artifact'),'server')
    def test_editor_is_not_packaged_server_acceptance(self):
        report=self.report();report['editor_build']=True;report['server_build']=False
        validate(report,'run',dict(manifest_sha256='artifact'),'editor-server')
        with self.assertRaises(ValueError):validate(report,'run',dict(manifest_sha256='artifact'),'server')

if __name__=='__main__':unittest.main()
