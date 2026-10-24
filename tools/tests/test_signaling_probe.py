import copy
import json
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_signaling_probe import server_bundle_id, validate_report
from run_mobile_probe import read_android_chunks


def good():
    peer = dict(connection=2, ice=3, dtls=3, protocol=1, sctp_open=True, local_sdp=True, remote_sdp=True,
                local_ice_complete=True, remote_ice_complete=True, queued=0, applied=2, local_udp=1, local_tcp=1,
                candidate_errors=0, last_candidate_error=0, stats_samples=3, stats_age_ms=10, pairs=2,
                succeeded_pairs=1, failed_pairs=0, stun_requests=2, stun_responses=2, send_discards=0)
    rows = [dict(index=i, mode='media' if i % 2 else 'data', delivery=('early_both','early_offer','trickle')[i % 3],
                 connected=True, closed=True, operations_accepted=14, operations_completed=14, duplicates=4,
                 early_offer_ice=2, early_answer_ice=2, early_offer_end=True, early_answer_end=True,
                 seconds=0.6, peers=[copy.deepcopy(peer),copy.deepcopy(peer)]) for i in range(8)]
    return dict(schema=1, run_id='run', platform='Android', editor_build=False, server_build=False, arm64_build=True,
                simulator_build=False, with_janus=False, artifact_manifest_sha256='digest',
                native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d', requested_pairs=8, completed_pairs=8,
                callbacks_on_game_thread=True, teardown_complete=True, network_transport_tested=True,
                network_media_tested=False, loopback_only=True, cross_process_tested=False, stun_turn_service_used=False,
                passed=True, error='', engine_version='5.8.3-test', pid=42, elapsed_seconds=8.0, pairs=rows)


class SignalingReportTests(unittest.TestCase):
    def validate(self, report): validate_report(report, 'run', 'Android', 'digest', False)
    def test_complete(self): self.validate(good())
    def test_server_bundle_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'Host.app/Contents/MacOS/HostServer'
            executable.parent.mkdir(parents=True)
            info = executable.parent.parent / 'Info.plist'
            def write(identifier, name='HostServer'):
                info.write_bytes(plistlib.dumps(dict(CFBundleIdentifier=identifier, CFBundleExecutable=name)))
            write('org.simurghrtc.HostServer')
            self.assertEqual(server_bundle_id(executable), 'org.simurghrtc.HostServer')
            for identifier in ('../escape', 'org.simurghrtc/escape', None):
                if identifier is None: info.write_bytes(plistlib.dumps({}))
                else: write(identifier)
                with self.assertRaises(ValueError): server_bundle_id(executable)
            write('org.simurghrtc.HostServer', 'OtherServer')
            with self.assertRaises(ValueError): server_bundle_id(executable)
            with self.assertRaises(ValueError): server_bundle_id(Path(directory) / 'HostServer')
    def test_wrong_identity_or_false_success(self):
        for key, value in [('run_id','stale'),('artifact_manifest_sha256','old'),('pid',True),('passed',False),
                           ('completed_pairs',7),('callbacks_on_game_thread',False),('teardown_complete',False),
                           ('server_build',True),('loopback_only',False),('elapsed_seconds',float('nan')),('error','failure')]:
            with self.subTest(key=key):
                report=good();report[key]=value
                with self.assertRaises(ValueError):self.validate(report)
    def test_incomplete_or_reordered_pairs(self):
        for mutate in [lambda r:r['pairs'].pop(),lambda r:r['pairs'].reverse(),lambda r:r['pairs'][0].update(closed=False),
                       lambda r:r['pairs'][0].update(operations_completed=13),lambda r:r['pairs'][0].update(early_answer_end=False),
                       lambda r:r['pairs'][0].update(duplicates=1),lambda r:r['pairs'][0].update(seconds=21)]:
            report=good();mutate(report)
            with self.assertRaises(ValueError):self.validate(report)
    def test_transport_not_established(self):
        for key,value in [('ice',4),('dtls',2),('protocol',0),('sctp_open',False),('queued',1),('applied',3),
                          ('stats_age_ms',1000),('stats_samples',0),('stun_responses',0),('succeeded_pairs',0)]:
            with self.subTest(key=key):
                report=good();report['pairs'][0]['peers'][0][key]=value
                with self.assertRaises(ValueError):self.validate(report)
    def test_signaling_chunks(self):
        raw=json.dumps(good());chunks=[raw[i:i+640] for i in range(0,len(raw),640)]
        log='\n'.join(f'SIMURGHRTC_SIGNALING_CHUNK run {i+1}/{len(chunks)} {chunk}' for i,chunk in enumerate(chunks))
        self.assertEqual(read_android_chunks(log,'run','SIGNALING',128),good())
        self.assertIsNone(read_android_chunks(log,'other','SIGNALING',128))
        self.assertIsNone(read_android_chunks(log.rsplit('\n',1)[0],'run','SIGNALING',128))
        with self.assertRaises(ValueError):read_android_chunks(log+'\n'+log.splitlines()[0],'run','SIGNALING',128)
        with self.assertRaises(ValueError):read_android_chunks(log,'run','SIGNALING',1)
