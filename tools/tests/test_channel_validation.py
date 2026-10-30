"""Packaged DataChannel acceptance must reject weak or mismatched evidence."""
import copy
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from channel_validation import CHANNEL_SCENARIOS, channel_case, validate_remote_channel
from network_fixtures import limit_sdp_messages
from run_data_example_probe import validate_example
import test_data_example_probe as examples


class ChannelValidationTests(unittest.TestCase):
    def report(self,scenario):
        c=channel_case(scenario);r=examples.DataExampleProbeTests().report()
        r.update(scenario=scenario,sent=c['messages'],received=c['messages'],rounds_completed=1,connection_errors=[],
                 max_tick_gap_seconds=.04,heartbeats=100)
        r['channel_validation']=dict(mode=c['mode'],phase=9,maximum_message_bytes=c['limit'],messages=c['messages'],
            text_messages=4,binary_messages=c['messages']-4,rejected_sends=5 if c['pressure'] else 3,burst_messages=32,
            byte_queue_rejections=int(c['pressure']),slot_queue_rejections=int(c['pressure']),
            byte_queue_recovered=c['pressure'],slot_queue_recovered=c['pressure'],options_checked=True,all_echoes_received=True,
            peak_buffered_bytes=262144 if c['pressure'] else c['limit'],buffer_events=12)
        return r

    def validate(self,r):
        validate_example(r,'run',42,'digest',True,'blueprint',r['scenario'])

    def test_requires_exact_payload_counts_limits_and_queue_recovery(self):
        for scenario in CHANNEL_SCENARIOS:
            good=self.report(scenario);self.validate(good)
            for key,value in [('phase',8),('messages',2),('maximum_message_bytes',0),('text_messages',0),('burst_messages',0),
                              ('options_checked',False),('all_echoes_received',False),('buffer_events',0),('peak_buffered_bytes',262145)]:
                bad=copy.deepcopy(good);bad['channel_validation'][key]=value
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):self.validate(bad)
        for key,value in [('byte_queue_recovered',False),('slot_queue_recovered',False),('byte_queue_rejections',0),
                          ('slot_queue_rejections',0),('peak_buffered_bytes',65536),('rejected_sends',3)]:
            bad=self.report('channels-0');bad['channel_validation'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.validate(bad)

    def test_remote_options_and_limits_must_match_the_requested_mode(self):
        for scenario in CHANNEL_SCENARIOS:
            c=channel_case(scenario);sid='00000000-0000-4000-8000-000000000002'
            options=dict(session_id=sid,label='sr107-channels-'+str(c['mode']),ordered=c['ordered'],
                         reliability=c['reliability'],reliability_value=c['value'],maximum_message_bytes=c['limit'],remote=True)
            server=dict(data_channels_opened=1,data_channels_closed=1,channel_options=[options],connected_session_ids=[sid],closed_session_ids=[sid])
            validate_remote_channel(server,scenario)
            for key,value in [('data_channels_opened',True),('data_channels_closed',False),('data_channels_closed',2)]:
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):
                    validate_remote_channel(dict(server,**{key:value}),scenario)
            for key,value in [('session_id','other'),('ordered',not c['ordered']),('reliability',99),('reliability_value',999),
                              ('maximum_message_bytes',0),('remote',False)]:
                bad=copy.deepcopy(server);bad['channel_options'][0][key]=value
                with self.subTest(scenario=scenario,key=key),self.assertRaises(ValueError):validate_remote_channel(bad,scenario)
            with self.assertRaises(ValueError):validate_remote_channel(dict(server,closed_session_ids=[]),scenario)

    def test_lower_limit_changes_only_the_sdp_attribute(self):
        sdp='v=0\r\na=fingerprint:sha-256 unchanged\r\na=max-message-size:65536\r\n'
        for body in [{'payload':{'description':{'sdp':sdp}}},{'events':[{'description':{'sdp':sdp}}]}]:
            original=json.dumps(body).encode();changed,count=limit_sdp_messages(original,32768)
            self.assertEqual(count,1);self.assertEqual(changed.decode().replace('32768','65536'),json.dumps(body,separators=(',',':')))
        original=b'{"operation":"poll","payload":{"cursor":1}}'
        self.assertEqual(limit_sdp_messages(original,32768),(original,0))
        for bad in [sdp.replace('a=max-message-size:65536\r\n',''),sdp+'a=max-message-size:65536\r\n']:
            with self.assertRaises(ValueError):limit_sdp_messages(json.dumps({'sdp':bad}).encode(),32768)
        with self.assertRaises(ValueError):limit_sdp_messages(original,65536)


if __name__=='__main__':unittest.main()
