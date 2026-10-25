import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_concurrent_probe import EXPECTED_RECEIVED, validate_client, validate_concurrent_server


class ConcurrentProbeTests(unittest.TestCase):
    def setUp(self):
        self.ids = {role: f'00000000-0000-4000-8000-{i:012d}' for i, role in
                    enumerate(('alpha', 'beta', 'gamma', 'delta', 'beta-rejoin'), 1)}

    def client(self, role='alpha', replacement=False, progress=False):
        phases = [1] if progress or (role == 'beta' and not replacement) else [2] if replacement else [1, 2]
        closed_by_server = role == 'beta' and not replacement
        return dict(schema=1, run_id='run', pid=123, server_build=False, editor_build=False,
                    arm64_build=True, platform='Mac', with_janus=True, artifact_manifest_sha256='digest',
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d', loopback_only=True,
                    network_media_tested=False, application_messages_tested=True, scenario='concurrent', passed=True,
                    engine_version='5.8.3-test', error='', native_closed=True, callbacks_on_game_thread=True,
                    remote_closed=not closed_by_server, close_reason='SessionClosed' if closed_by_server else '',
                    concurrent=dict(role=role, replacement=replacement, callbacks_on_game_thread=True,
                                    unexpected_messages=False, session_id=self.ids[role+'-rejoin' if replacement else role],
                                    phases=[dict(phase=p, received=EXPECTED_RECEIVED[p][role],
                                                 expected=EXPECTED_RECEIVED[p][role], sealed=True) for p in phases]))

    def validate(self, report, role='alpha', replacement=False, progress=False):
        return validate_client(report, 'run', 123, 'digest', True, role, replacement, progress)

    def server(self):
        return dict(created=5, connected=5, closed=5, expired=1, live_peers=0, records=0,
                    connected_session_ids=list(self.ids.values()), closed_session_ids=list(self.ids.values()),
                    connected_sessions=0, peak_live_peers=4, peak_connected_sessions=4,
                    concurrent=dict(ready=0, phase=2, sealed=3, native_closed=5, isolation_checks=9,
                                    callbacks_on_game_thread=True,
                                    sessions=[dict(session_id=sid, principal=case.split('-')[0], state=3,
                                                   native_closed=True) for case, sid in self.ids.items()],
                                    phases=[dict(phase=p, clients=len(cases), echoes=echoes, routed=routed,
                                                 denied=denied, sealed_sessions=[self.ids[c] for c in cases])
                                            for p, cases, echoes, routed, denied in
                                            ((1, ('alpha','beta','gamma','delta'), 8, 8, 8),
                                             (2, ('alpha','beta-rejoin','gamma'), 6, 3, 6))]))

    def test_each_client_role_and_replacement_has_exact_phase_evidence(self):
        for role, replacement, progress in [('alpha',False,False), ('beta',False,False),
                                            ('gamma',False,False), ('delta',False,True), ('beta',True,False)]:
            with self.subTest(role=role, replacement=replacement):
                report = self.client(role, replacement, progress)
                if progress: del report['scenario']
                self.validate(report, role, replacement, progress)
                for key, value in [('received', 0), ('expected', 0), ('sealed', False), ('phase', 3)]:
                    bad = copy.deepcopy(report); bad['concurrent']['phases'][0][key] = value
                    with self.assertRaises(ValueError): self.validate(bad, role, replacement, progress)

    def test_client_requires_isolation_owner_thread_and_native_cleanup(self):
        report = self.client()
        for key, value in [('native_closed',False), ('remote_closed',False), ('pid',124),
                           ('artifact_manifest_sha256','other'), ('callbacks_on_game_thread',False)]:
            bad = copy.deepcopy(report); bad[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.validate(bad)
        for key, value in [('unexpected_messages',True), ('callbacks_on_game_thread',False), ('replacement',True)]:
            bad = copy.deepcopy(report); bad['concurrent'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.validate(bad)

    def test_server_requires_simultaneous_connections_and_full_cleanup(self):
        report = self.server(); validate_concurrent_server(report, self.ids)
        for key, value in [('peak_connected_sessions',3), ('peak_live_peers',3), ('closed',4),
                           ('expired',0), ('live_peers',1), ('records',1), ('connected_sessions',1)]:
            bad = copy.deepcopy(report); bad[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_concurrent_server(bad,self.ids)
        for key in ('native_closed','isolation_checks','sealed'):
            bad = copy.deepcopy(report); bad['concurrent'][key] -= 1
            with self.subTest(key=key), self.assertRaises(ValueError): validate_concurrent_server(bad,self.ids)

    def test_server_rejects_wrong_principal_and_reused_or_missing_sessions(self):
        report = self.server()
        for key, value in [('principal','gamma'), ('native_closed',False), ('state',2)]:
            bad = copy.deepcopy(report); bad['concurrent']['sessions'][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_concurrent_server(bad,self.ids)
        bad_ids = dict(self.ids); bad_ids['beta-rejoin'] = self.ids['beta']
        with self.assertRaises(ValueError): validate_concurrent_server(report,bad_ids)
        bad_ids = dict(self.ids); del bad_ids['delta']
        with self.assertRaises(ValueError): validate_concurrent_server(report,bad_ids)
        bad = copy.deepcopy(report); bad['closed_session_ids'].pop()
        with self.assertRaises(ValueError): validate_concurrent_server(bad,self.ids)

    def test_phase_barriers_require_the_right_participants(self):
        report = self.server()
        bad = copy.deepcopy(report); bad['concurrent']['phases'][1]['sealed_sessions'][1] = self.ids['beta']
        with self.assertRaises(ValueError): validate_concurrent_server(bad,self.ids)
        for key in ('echoes','routed','denied','clients'):
            bad = copy.deepcopy(report); bad['concurrent']['phases'][0][key] -= 1
            with self.subTest(key=key), self.assertRaises(ValueError): validate_concurrent_server(bad,self.ids)
