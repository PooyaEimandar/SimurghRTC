from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_session_probe import LIFECYCLES, validate_session


class SessionProbeTests(unittest.TestCase):
    def report(self, scenario='data', role=0, ready=False):
        silent=scenario in ('owner-loss','world','session-gc')
        count=2 if ready else 3 if scenario=='data' else 1
        report=dict(schema=1,run_id='run',pid=42,scenario=scenario,role=role,editor_build=False,server_build=False,
                    arm64_build=True,platform='Mac',with_janus=True,artifact_manifest_sha256='digest',
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',shared_api=True,
                    loopback_only=True,network_media_tested=False,passed=True,error='',ready=ready,engine_version='5.8.3-test',
                    phase=2 if ready else 100,quiet_ticks=0 if ready else 8,pending_cleanup=0,transports_created=count,
                    native_closed=0 if ready else count,remote_closed=0 if ready else count,isolation_checks=0 if ready or scenario!='data' else 6,
                    backpressure_tested=scenario=='data' and role==0,backpressure_observed=scenario=='data' and role==0,
                    backpressure_recovered=scenario=='data' and role==0,clients=[])
        for index in range(2 if scenario=='data' else 1):
            operations=2 if ready else (8 if index==0 else 4) if scenario=='data' else 2 if silent or scenario=='disconnect-pending' else 1
            received=0 if ready or scenario!='data' else (9 if role==0 else 4) if index==0 else 2
            report['clients'].append(dict(facade_id=f'00000000-0000-4000-8000-{index+1:012d}',received=received,unexpected=0,
                operations=operations,completions=1 if silent else operations,last_error=9 if silent or scenario=='cancel' else 8 if scenario in ('timeout','http-timeout') else 0,
                callbacks_on_game_thread=True,expected_empty=True,triggered=silent or scenario=='reentrant',session_collected=scenario=='session-gc'))
        return report

    def validate(self, report):
        return validate_session(report,'run',42,'digest',True,report['scenario'],report['role'],report['ready'])

    def test_all_scenarios_and_simultaneous_readiness(self):
        for scenario in ('data',*LIFECYCLES):self.validate(self.report(scenario))
        for role in range(4):
            self.validate(self.report(role=role));self.validate(self.report(role=role,ready=True))

    def test_requires_exact_delivery_isolation_and_cleanup(self):
        for key,value in [('native_closed',2),('native_closed',True),('pending_cleanup',1),('quiet_ticks',0),
                          ('isolation_checks',5),('remote_closed',2),('shared_api',False),('backpressure_recovered',False)]:
            bad=self.report();bad[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.validate(bad)
        for key,value in [('received',8),('unexpected',1),('expected_empty',False),('callbacks_on_game_thread',False),('completions',7)]:
            bad=self.report();bad['clients'][0][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.validate(bad)

    def test_rejects_shared_identity_and_premature_readiness(self):
        bad=self.report();bad['clients'][1]['facade_id']=bad['clients'][0]['facade_id']
        with self.assertRaises(ValueError):self.validate(bad)
        bad=self.report(ready=True);bad['clients'][0]['completions']=1
        with self.assertRaises(ValueError):self.validate(bad)
        bad=self.report(ready=True);bad['native_closed']=1
        with self.assertRaises(ValueError):self.validate(bad)

    def test_silent_teardown_and_collection_are_required(self):
        for scenario in ('owner-loss','world','session-gc'):
            for key,value in [('completions',2),('last_error',0),('triggered',False)]:
                bad=self.report(scenario);bad['clients'][0][key]=value
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):self.validate(bad)
        bad=self.report('session-gc');bad['clients'][0]['session_collected']=False
        with self.assertRaises(ValueError):self.validate(bad)

    def test_cancel_and_timeout_are_distinct_from_success(self):
        for scenario in ('cancel','timeout','http-timeout'):
            bad=self.report(scenario);bad['clients'][0]['last_error']=0
            with self.subTest(scenario=scenario),self.assertRaises(ValueError):self.validate(bad)
