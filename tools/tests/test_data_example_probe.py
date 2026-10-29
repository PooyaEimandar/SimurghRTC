from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_data_example_probe import MAPS, SCENARIOS, validate_example


class DataExampleProbeTests(unittest.TestCase):
    def report(self, implementation='blueprint', scenario='messages'):
        pending=scenario.startswith('destroy-');messages=scenario=='messages'
        outcomes={'messages':['Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
                  'timeout':['Connect:Failure','Disconnect:Success'], 'disconnect-pending':['Connect:Cancelled','Disconnect:Success'],
                  'destroy-connect':[], 'destroy-open':['Connect:Success']}[scenario]
        states={'messages':[1,2,3,4], 'timeout':[1,5,3,4], 'disconnect-pending':[1,3,4], 'destroy-connect':[1], 'destroy-open':[1,2]}[scenario]
        return dict(schema=1,run_id='run',pid=42,implementation=implementation,scenario=scenario,map=MAPS[implementation],
            editor_build=False,server_build=False,arm64_build=True,with_janus=True,artifact_manifest_sha256='digest',
            native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',shared_api=True,loopback_only=True,
            network_media_tested=False,passed=True,error='',transports_created=1,native_closed=1,remote_closed=1,pending_cleanup=0,
            objects_collected=True,end_play_calls=1,end_play_closed=True,late_callbacks=0,callbacks_on_game_thread=True,
            disconnect_requested=scenario=='disconnect-pending',
            actor_class='/Script/ContractHost.ContractDataCpp' if implementation=='cpp' else '/Game/Examples/Direct/BP_RtcData.BP_RtcData_C',
            engine_version='5.8.3-test',session_id='00000000-0000-4000-8000-000000000001',
            operations=len(outcomes)+int(pending),completions=len(outcomes),outcomes=outcomes,states=states,
            sent=2 if messages else 0,received=2 if messages else 0,channel_events=2 if messages else 0,open_events=1 if messages else 0,
            quiet_ticks=8,last_failure=8 if scenario=='timeout' else 9 if scenario=='disconnect-pending' else 0,
            pending_at_destruction=pending,silent_cancelled=pending,exact_payloads=messages,channel_info_checked=messages)

    def validate(self, report):
        return validate_example(report,'run',42,'digest',True,report['implementation'],report['scenario'])

    def test_accepts_both_maps_and_all_lifecycle_cases(self):
        for implementation in MAPS:
            for scenario in SCENARIOS:self.validate(self.report(implementation,scenario))

    def test_rejects_wrong_actor_map_or_run(self):
        for key,value in [('actor_class','/Script/ContractHost.ContractDataCpp'),('map',MAPS['cpp']),('pid',43),('run_id','old'),
                          ('editor_build',True),('artifact_manifest_sha256','old')]:
            bad=self.report();bad[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.validate(bad)

    def test_requires_actual_messages_and_events(self):
        for key,value in [('received',0),('exact_payloads',False),('channel_info_checked',False),('open_events',0),
                          ('states',[]),('states',[1,True,3,4]),('outcomes',['Connect:Success']),('completions',5)]:
            bad=self.report();bad[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.validate(bad)

    def test_requires_silent_pending_owner_cleanup(self):
        for scenario in ('destroy-connect','destroy-open'):
            for key,value in [('pending_at_destruction',False),('silent_cancelled',False),('end_play_closed',False),
                              ('end_play_calls',0),('objects_collected',False),('late_callbacks',1),('native_closed',False),('pending_cleanup',1)]:
                bad=self.report(scenario=scenario);bad[key]=value
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):self.validate(bad)

    def test_timeout_and_cancel_cannot_claim_success(self):
        for scenario in ('timeout','disconnect-pending'):
            bad=self.report(scenario=scenario);bad['last_failure']=0
            with self.subTest(scenario=scenario),self.assertRaises(ValueError):self.validate(bad)


if __name__=='__main__':unittest.main()
