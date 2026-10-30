"""Reject incomplete expiry, aggregate-pressure and impairment evidence."""
import copy
from pathlib import Path
import sys
import unittest
import uuid
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from network_validation import NETWORK_SCENARIOS,network_case,validate_network,validate_remote_network,validate_impairment
from run_data_example_probe import validate_example
import test_data_example_probe as examples


class NetworkValidationTests(unittest.TestCase):
    def report(self,scenario):
        c=network_case(scenario);r=examples.DataExampleProbeTests().report()
        outcomes=['Connect:Success']+['OpenChannel:Success']*c['channels']+['CloseChannel:Success']*c['channels']+['Disconnect:Success']
        r.update(scenario=scenario,sent=c['sent'],received=c['received'],outcomes=outcomes,operations=len(outcomes),completions=len(outcomes),
                 rounds_completed=1,connection_errors=[],channel_ids=[str(uuid.uuid4()) for _ in range(c['channels'])],max_tick_gap_seconds=.05,heartbeats=100)
        r['network_validation']=dict(quiet_seconds=10.1 if c['expiry'] else 2.1,mode=c['mode'],phase=9,channels=c['channels'],channels_closed=c['channels'],messages=c['received'],
            expired_admissions=8 if c['expiry'] else 0,rejected_sends=int(c['multi']),peak_buffered_bytes=1048576 if c['multi'] else 8192,
            empty_channel_at_rejection=0 if c['multi'] else -1,aggregate_queue_recovered=c['multi'],all_expected_received=True,buffer_events=10)
        return r

    def test_shared_validation_requires_exact_counts_and_all_channel_closures(self):
        for scenario in NETWORK_SCENARIOS:
            r=self.report(scenario);validate_example(r,'run',42,'digest',True,'blueprint',scenario)
            for key,value in [('phase',3),('quiet_seconds',0),('messages',0),('channels_closed',0),('expired_admissions',99),
                              ('peak_buffered_bytes',1048577),('all_expected_received',False)]:
                bad=copy.deepcopy(r);bad['network_validation'][key]=value
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):validate_network(bad,scenario)
        for key,value in [('empty_channel_at_rejection',65536),('aggregate_queue_recovered',False),('rejected_sends',0),('peak_buffered_bytes',262144)]:
            bad=self.report('network-6');bad['network_validation'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):validate_network(bad,'network-6')

    def test_remote_options_include_the_short_lifetime_and_every_channel(self):
        for scenario in NETWORK_SCENARIOS:
            c=network_case(scenario);sid=str(uuid.uuid4())
            rows=[dict(session_id=sid,label=f"sr107-network-{c['mode']}-{i}",ordered=c['ordered'],reliability=c['reliability'],
                       reliability_value=c['value'],maximum_message_bytes=65536,remote=True) for i in range(c['channels'])]
            r=dict(connected_session_ids=[sid],closed_session_ids=[sid],channel_options=rows,data_channels_opened=c['channels'],data_channels_closed=c['channels'])
            validate_remote_network(r,scenario)
            r['channel_options'][0]['reliability_value']=999
            with self.assertRaises(ValueError):validate_remote_network(r,scenario)

    def test_packet_counters_must_prove_the_requested_impairment(self):
        for scenario in NETWORK_SCENARIOS:
            c=network_case(scenario)
            d=dict(packets=50,eligible=40,impaired=0 if c['multi'] else 5 if c['expiry'] else 40,
                   dropped=0 if c['multi'] else 5,reordered=10 if c['mode']<2 else 0,forwarded=45)
            r=dict(closed=True,error='',pending_packets=0,flows=1,peak_held_packets=1,directions=dict(outbound=dict(d),inbound=dict(d)))
            validate_impairment(r,scenario)
            for key,value in [('closed',False),('pending_packets',1),('error','lost socket'),('flows',9),('peak_held_packets',17)]:
                bad=copy.deepcopy(r);bad[key]=value
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):validate_impairment(bad,scenario)
            bad=copy.deepcopy(r);bad['directions']['outbound']['eligible']=0
            with self.assertRaises(ValueError):validate_impairment(bad,scenario)


if __name__=='__main__':unittest.main()
