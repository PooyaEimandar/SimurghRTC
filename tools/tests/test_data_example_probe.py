from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_data_example_probe import MAPS, SCENARIOS, RECOVERY_SCENARIOS, validate_example


class DataExampleProbeTests(unittest.TestCase):
    def report(self, implementation='blueprint', scenario='messages'):
        pending=scenario.startswith('destroy-');messages=scenario=='messages'
        outcomes={'messages':['Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
                  'timeout':['Connect:Failure','Disconnect:Success'], 'disconnect-pending':['Connect:Cancelled','Disconnect:Success'],
                  'destroy-connect':[], 'destroy-open':['Connect:Success']}[scenario]
        states={'messages':[1,2,3,4], 'timeout':[1,5,3,4], 'disconnect-pending':[1,3,4], 'destroy-connect':[1], 'destroy-open':[1,2]}[scenario]
        return dict(schema=1,run_id='run',pid=42,implementation=implementation,scenario=scenario,map=MAPS[implementation],
            platform='Mac',simulator_build=False,editor_build=False,server_build=False,arm64_build=True,with_janus=True,artifact_manifest_sha256='digest',
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

    def recovery_report(self, scenario):
        value=self.report();graceful=scenario=='reconnect'
        value.update(scenario=scenario,transports_created=2,native_closed=2,remote_closed=2 if graceful else 1,
                     sent=4,received=4,rounds_completed=2,stale_handle_checks=6,recovery_requested=not graceful,
                     channel_ids=['00000000-0000-4000-8000-000000000002','00000000-0000-4000-8000-000000000003'],
                     connection_errors=[] if graceful else [11],heartbeats=300,max_tick_gap_seconds=.04,
                     states=[1,2,3,4,1,2,3,4] if graceful else [1,2,5,1,2,3,4])
        value['outcomes']=(value['outcomes'] if graceful else value['outcomes'][:2])+value['outcomes']
        value['operations']=value['completions']=len(value['outcomes'])
        return value

    def test_recovery_requires_two_distinct_rounds_and_responsive_cleanup(self):
        for scenario in RECOVERY_SCENARIOS:
            good=self.recovery_report(scenario);self.validate(good)
            for key,value in [('sent',2),('rounds_completed',1),('stale_handle_checks',3),('native_closed',1),
                              ('channel_ids',good['channel_ids'][:1]*2),('max_tick_gap_seconds',2),('heartbeats',0),
                              ('connection_errors',[8]),('recovery_requested',scenario=='reconnect')]:
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):self.validate(dict(good,**{key:value}))

    def background_report(self, pending):
        value=self.recovery_report('reconnect')
        value.update(scenario='background-connect' if pending else 'background-connected',inactive_events=1,active_events=1,background_events=1,foreground_events=1,
            background_shutdown=True,background_pending=pending,background_cancelled=pending,retired_objects_collected=True,
            background_seconds=8.5,shutdown_seconds=.002,session_ids=['00000000-0000-4000-8000-000000000004',value['session_id']],
            rounds_completed=1 if pending else 2,stale_handle_checks=0 if pending else 6,
            states=[1,1,2,3,4] if pending else [1,2,1,2,3,4],remote_closed=1)
        value['outcomes']=value['outcomes'][4:] if pending else value['outcomes'][:2]+value['outcomes'][4:]
        value['operations']=len(value['outcomes'])+int(pending);value['completions']=len(value['outcomes'])
        if pending:value.update(sent=2,received=2,channel_ids=value['channel_ids'][1:])
        return value

    def test_background_requires_os_events_silent_shutdown_and_fresh_session(self):
        for pending in (False,True):
            good=self.background_report(pending);self.validate(good)
            for key,value in [('inactive_events',0),('active_events',2),('background_events',0),('foreground_events',2),('background_shutdown',False),
                ('background_pending',not pending),('background_cancelled',not pending),('retired_objects_collected',False),
                ('session_ids',[good['session_id']]*2),('shutdown_seconds',1),('background_seconds',0),('late_callbacks',1)]:
                with self.subTest(pending=pending,key=key),self.assertRaises(ValueError):self.validate(dict(good,**{key:value}))


if __name__=='__main__':unittest.main()
