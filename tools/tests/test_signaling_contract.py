"""Offline protocol scenarios; live HTTP/native coverage is recorded separately."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
import signaling_contract as contract

FIXTURES = Path(__file__).parents[2] / 'Fixtures/signaling/v1'


class DirectContractTests(unittest.TestCase):
    def setUp(self):
        self.trace = json.loads((FIXTURES / 'direct-session.json').read_text())
        self.model = contract.DirectFixtureModel()
        self.model.apply(self.trace['steps'][0]['request'])

    def request(self, kind, body, **changes):
        value = {'protocol': contract.PROTOCOL, 'version': 1, 'type': kind, 'body': body,
                 'request_id': f'req-{self.model.next_sequence:04d}', 'sequence': self.model.next_sequence,
                 'session_id': contract.SESSION}
        value.update(changes)
        return value

    def failure(self, request, code):
        with self.assertRaises(contract.ContractError) as error:
            self.model.apply(request)
        self.assertEqual(error.exception.code, code)

    def test_versioned_transcript_matches_oracle(self):
        model = contract.DirectFixtureModel()
        for step in self.trace['steps']:
            if 'answer_fixture' in step:
                model.record_answer_fixture(step['answer_fixture'])
            else:
                self.assertEqual(step['response'], model.apply(step['request']))

    def test_duplicate_is_idempotent_and_isolated_from_caller_mutation(self):
        original = self.trace['steps'][0]['request']
        first = self.model.apply(original)
        first['body']['state'] = 'tampered'
        second = self.model.apply(dict(reversed(list(original.items()))))
        self.assertEqual(second['body']['state'], 'open')
        self.assertEqual(self.model.next_sequence, 1)

    def test_duplicate_conflict_does_not_advance_state(self):
        changed = copy.deepcopy(self.trace['steps'][0]['request'])
        changed['body']['identity'] = 'different'
        self.failure(changed, 'RequestConflict')
        self.assertEqual(self.model.next_sequence, 1)

    def test_sequence_gap_and_wrong_session(self):
        self.failure(self.request('poll', {'after_event': 0}, sequence=2), 'OutOfOrder')
        self.failure(self.request('poll', {'after_event': 0}, session_id='22222222-2222-4222-8222-222222222222'), 'SessionNotFound')
        self.assertEqual(self.model.next_sequence, 1)

    def test_rejects_version_extra_fields_bool_and_float_indices(self):
        for changes, code in [({'version': 2}, 'UnsupportedVersion'), ({'version': True}, 'UnsupportedVersion'),
                              ({'extra': 1}, 'InvalidArgument'), ({'sequence': True}, 'InvalidArgument'),
                              ({'sequence': 1.0}, 'InvalidArgument')]:
            with self.subTest(changes=changes):
                self.failure(self.request('poll', {'after_event': 0}, **changes), code)

    def test_utf8_and_sdp_limits(self):
        create = copy.deepcopy(self.trace['steps'][0]['request'])
        create['body']['identity'] = 'é' * 65
        with self.assertRaises(contract.ContractError):
            contract.validate_request(create)
        for sdp in ['v=0\n', 'v=0\r\n' + 'x' * contract.MAX_SDP_BYTES + '\r\n', 'v=0\r\n\x00\r\n', 'v=0\r\nx\ry\r\n', 'v=0\r\n\ud800\r\n']:
            self.failure(self.request('offer', {'revision': 1, 'sdp': sdp}), 'InvalidArgument')

    def test_early_ice_flushes_only_after_offer_and_end_is_terminal(self):
        candidate = self.trace['steps'][1]['request']['body']
        self.model.apply(self.request('ice', candidate))
        self.assertEqual(self.model.pending_ice, [candidate])
        self.assertEqual(self.model.applied_ice, [])
        self.model.apply(self.request('offer', self.trace['steps'][2]['request']['body']))
        self.assertEqual(self.model.pending_ice, [])
        self.assertEqual(self.model.applied_ice, [candidate])
        self.model.apply(self.request('ice', {'revision': 1, 'completed': True}))
        self.failure(self.request('ice', candidate), 'IceComplete')

    def test_candidate_shape_and_initial_revision(self):
        body = copy.deepcopy(self.trace['steps'][1]['request']['body'])
        for changed in [dict(body, completed=True), dict(body, sdp_mline_index=True), dict(body, revision=2),
                        dict(body, candidate='candidate:x\r\ninjected')]:
            self.failure(self.request('ice', changed), 'InvalidArgument')

    def test_candidate_queue_overflow_is_explicit_and_bounded(self):
        for n in range(contract.MAX_ICE):
            body = dict(self.trace['steps'][1]['request']['body'], candidate=f'candidate:{n} 1 UDP 1 192.0.2.1 {50000+n} typ host')
            self.model.apply(self.request('ice', body))
        self.failure(self.request('ice', self.trace['steps'][1]['request']['body']), 'ResourceLimit')
        self.assertEqual(len(self.model.pending_ice), contract.MAX_ICE)

    def test_replay_capacity_never_evicts_and_reexecutes_old_requests(self):
        for _ in range(contract.MAX_REPLAY):
            self.model.apply(self.request('poll', {'after_event': 0}))
        self.failure(self.request('poll', {'after_event': 0}), 'ResourceLimit')
        self.assertEqual(len(self.model.replay), contract.MAX_REPLAY + 1)
        self.assertEqual(self.model.apply(self.trace['steps'][0]['request']), self.trace['steps'][0]['response'])

    def test_close_bypasses_capacity_and_unknown_last_sequence(self):
        first_poll = self.request('poll', {'after_event': 0})
        self.model.apply(first_poll)
        for _ in range(contract.MAX_REPLAY - 1): self.model.apply(self.request('poll', {'after_event': 0}))
        close = self.request('close', {}, sequence=0)
        result = self.model.apply(close)
        self.assertEqual(result['body']['state'], 'closed')
        self.assertEqual(self.model.apply(close), result)
        self.failure(dict(first_poll, sequence=self.model.next_sequence), 'SessionClosed')

    def test_poll_cursor_and_close_cleanup(self):
        self.model.apply(self.request('offer', self.trace['steps'][2]['request']['body']))
        self.model.record_answer_fixture(self.trace['steps'][3]['answer_fixture'])
        self.failure(self.request('poll', {'after_event': 2}), 'InvalidCursor')
        response = self.model.apply(self.request('poll', {'after_event': 0}))
        self.assertEqual(response['body']['events'][0]['event_id'], 1)
        self.assertEqual(self.model.apply(self.request('poll', {'after_event': 1}))['body']['events'], [])
        close = self.request('close', {})
        result = self.model.apply(close)
        self.assertEqual(self.model.apply(close), result)
        self.assertEqual(self.model.events + self.model.pending_ice + self.model.applied_ice, [])
        self.failure(self.request('poll', {'after_event': 0}), 'SessionClosed')


class JanusContractTests(unittest.TestCase):
    def setUp(self):
        trace = json.loads((FIXTURES / 'janus-videoroom.json').read_text())
        self.ack = trace['messages'][5]['body']
        self.event = trace['messages'][6]['body']
        self.pending = contract.JanusTransaction('join-v1', trace['session_id'], trace['handle_id'], trace['plugin'])

    def test_ack_is_not_completion_and_duplicate_event_is_ignored(self):
        self.assertTrue(self.pending.consume(self.ack))
        self.assertTrue(self.pending.acknowledged)
        self.assertIsNone(self.pending.result)
        self.assertTrue(self.pending.consume(self.event))
        self.assertFalse(self.pending.consume(self.event))

    def test_wrong_session_handle_plugin_transaction_and_unsolicited_event(self):
        for key, value in [('session_id', 99), ('session_id', 1001.0), ('sender', 99), ('sender', 1003.0), ('transaction', 'other'),
                           ('plugindata', {'plugin': 'janus.plugin.echotest', 'data': {}})]:
            with self.subTest(key=key):
                self.assertFalse(self.pending.consume(dict(self.event, **{key: value})))
        unsolicited = dict(self.event)
        del unsolicited['transaction']
        self.assertFalse(self.pending.consume(unsolicited))
        self.assertIsNone(self.pending.result)

    def test_error_completes_without_ack(self):
        error = {'janus': 'error', 'transaction': 'join-v1', 'session_id': 1001, 'error': {'code': 458, 'reason': 'Gone'}}
        self.assertTrue(self.pending.consume(error))
        self.assertEqual(self.pending.result['error']['code'], 458)
        self.assertFalse(self.pending.consume(self.ack))

    def test_integer_ids_do_not_pass_through_float(self):
        # Parser stress case, not an assertion about Janus's generated ID range.
        session, handle = 2**53 + 1, 2**53 + 3
        pending = contract.JanusTransaction('join-v1', session, handle, 'janus.plugin.videoroom')
        event = json.loads(json.dumps(dict(self.event, session_id=session, sender=handle)))
        self.assertTrue(pending.consume(event))
        self.assertEqual(pending.result['sender'], handle)

    def test_jsep_examples_use_crlf_and_janus_candidate_names(self):
        trace = json.loads((FIXTURES / 'janus-jsep.json').read_text())
        contract.validate_sdp(trace['request']['jsep']['sdp'])
        self.assertEqual(set(trace['candidate']['candidate']), {'candidate', 'sdpMid', 'sdpMLineIndex'})
        self.assertEqual(trace['completion']['candidate'], {'completed': True})


if __name__ == '__main__':
    unittest.main()
