import copy
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_data_probe import validate_data, validate_server_data

class DataProbeTests(unittest.TestCase):
    def test_requires_complete_round_trip_and_backpressure_evidence(self):
        data = dict(phase=0, modes=6, closed_channels=6, messages=73, text_messages=18, binary_messages=55,
                    rejected_sends=25, ordered_burst=32, backpressure_observed=True, writable_resumed=True,
                    callbacks_on_game_thread=True, all_echoes_received=True)
        report = dict(pairs=[dict(data=data)])
        validate_data(report)
        for key in data:
            with self.subTest(key=key):
                bad = copy.deepcopy(report)
                del bad['pairs'][0]['data'][key]
                with self.assertRaises(ValueError): validate_data(bad)
                bad = copy.deepcopy(report)
                bad['pairs'][0]['data'][key] = False if type(data[key]) is bool else data[key]-1
                with self.assertRaises(ValueError): validate_data(bad)

    def test_peer_close_can_cancel_last_remote_close_callback(self):
        report = dict(data_channels_opened=24, data_channels_closed=23, data_received=292, data_sent=292, echo_pending=0)
        validate_server_data(report, 4)
        for key, value in [('data_channels_opened',23),('data_channels_closed',19),('data_channels_closed',25),
                           ('data_received',291),('data_sent',291),('echo_pending',1),('data_channels_closed',True)]:
            bad = dict(report); bad[key] = value
            with self.subTest(key=key, value=value):
                with self.assertRaises(ValueError): validate_server_data(bad, 4)
