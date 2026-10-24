"""Version 1 fixture oracle. No sockets, transport, SDP negotiation or production session implementation."""
from __future__ import annotations
import copy
import json
import re

MAX_REQUEST_BYTES = 262144
MAX_SDP_BYTES = 131072
MAX_ICE = 32
MAX_REPLAY = 128
MAX_EVENTS = 64
PROTOCOL = 'simurghrtc.direct'
SESSION = '11111111-1111-4111-8111-111111111111'


class ContractError(ValueError):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def require(condition, code='InvalidArgument'):
    if not condition:
        raise ContractError(code)


def exact(obj, keys):
    require(type(obj) is dict and set(obj) == set(keys))


def text(value, maximum):
    require(type(value) is str)
    try:
        size = len(value.encode('utf-8'))
    except UnicodeEncodeError:
        raise ContractError('InvalidArgument') from None
    require(0 < size <= maximum and '\x00' not in value)


def integer(value, low, high):
    require(type(value) is int and low <= value <= high)


def validate_sdp(sdp):
    text(sdp, MAX_SDP_BYTES)
    lines = sdp.replace('\r\n', '')
    require(sdp.startswith('v=0\r\n') and sdp.endswith('\r\n') and '\n' not in lines and '\r' not in lines)
    # Only framing/limits here. the live native peer performs full SDP parsing.


def validate_request(request):
    require(type(request) is dict)
    require(request.get('protocol') == PROTOCOL and type(request.get('version')) is int and request['version'] == 1, 'UnsupportedVersion')
    kind = request.get('type')
    require(kind in ('create', 'offer', 'ice', 'poll', 'close'))
    fields = {'protocol', 'version', 'request_id', 'sequence', 'type', 'body'}
    if kind != 'create':
        fields.add('session_id')
    exact(request, fields)
    text(request['request_id'], 64)
    require(re.fullmatch(r'[A-Za-z0-9_-]+', request['request_id']) is not None)
    integer(request['sequence'], 0, 2**31 - 1)
    try:
        size = len(json.dumps(request, ensure_ascii=False, separators=(',', ':')).encode())
    except (UnicodeEncodeError, TypeError, ValueError):
        raise ContractError('InvalidArgument') from None
    require(size <= MAX_REQUEST_BYTES, 'PayloadTooLarge')
    if kind != 'create':
        require(type(request['session_id']) is str and re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', request['session_id']) is not None)
    body = request['body']
    if kind == 'create':
        exact(body, ['purpose', 'identity'])
        require(body['purpose'] in ('data', 'call', 'text-room'))
        text(body['identity'], 128)
    elif kind == 'offer':
        exact(body, ['revision', 'sdp'])
        integer(body['revision'], 1, 1)  # Only the initial offer is supported.
        validate_sdp(body['sdp'])
    elif kind == 'ice':
        require(type(body) is dict)
        if 'completed' in body:
            exact(body, ['revision', 'completed'])
            require(body['completed'] is True)
        else:
            exact(body, ['revision', 'candidate', 'sdp_mid', 'sdp_mline_index', 'username_fragment'])
            text(body['candidate'], 2048)
            require(body['candidate'].startswith('candidate:') and '\n' not in body['candidate'] and '\r' not in body['candidate'])
            text(body['sdp_mid'], 32)
            integer(body['sdp_mline_index'], 0, 31)
            text(body['username_fragment'], 256)
        integer(body['revision'], 1, 1)
    elif kind == 'poll':
        exact(body, ['after_event'])
        integer(body['after_event'], 0, 2**31 - 1)
    else:
        exact(body, [])


class DirectFixtureModel:
    """Bounded offline request semantics. Accepted means recorded, never RTC connected."""
    def __init__(self, session_id=SESSION):
        self.session_id = session_id
        self.state = 'new'
        self.next_sequence = 0
        self.replay = {}
        self.create_id = None
        self.pending_ice = []
        self.applied_ice = []
        self.ice_complete = False
        self.events = []

    def apply(self, request):
        validate_request(request)
        encoded = json.dumps(request, sort_keys=True, separators=(',', ':'))
        request_id = request['request_id']
        if request_id in self.replay:
            prior, result = self.replay[request_id]
            require(prior == encoded, 'RequestConflict')
            return copy.deepcopy(result)
        kind, body = request['type'], request['body']
        if kind != 'close':
            require(len(self.replay) - int(self.create_id in self.replay) < MAX_REPLAY, 'ResourceLimit')
            require(request['sequence'] == self.next_sequence, 'OutOfOrder')
        if kind == 'create':
            require(self.state == 'new', 'InvalidState')
            self.state = 'open'
            self.create_id = request_id
            result = {'state': self.state}
        else:
            require(request['session_id'] == self.session_id and self.state != 'new', 'SessionNotFound')
            require(self.state != 'closed', 'SessionClosed')
            if kind == 'offer':
                require(self.state == 'open', 'InvalidState')
                self.state = 'offer-set'
                self.applied_ice.extend(self.pending_ice)
                self.pending_ice.clear()
                result = {'state': self.state, 'applied_ice': len(self.applied_ice)}
            elif kind == 'ice':
                require(not self.ice_complete, 'IceComplete')
                require(len(self.pending_ice) + len(self.applied_ice) < MAX_ICE, 'ResourceLimit')
                self.ice_complete = body.get('completed', False)
                destination = self.applied_ice if self.state == 'offer-set' else self.pending_ice
                destination.append(copy.deepcopy(body))
                result = {'pending_ice': len(self.pending_ice), 'applied_ice': len(self.applied_ice)}
            elif kind == 'poll':
                require(body['after_event'] <= len(self.events), 'InvalidCursor')
                result = {'events': copy.deepcopy(self.events[body['after_event']:]), 'last_event': len(self.events)}
            else:
                self.state = 'closed'
                self.pending_ice.clear()
                self.applied_ice.clear()
                self.events.clear()
                # Close invalidates outstanding results, while create and close retries stay idempotent.
                self.replay = {self.create_id: self.replay[self.create_id]}
                result = {'state': self.state}
        response = {'protocol': PROTOCOL, 'version': 1, 'type': 'result', 'request_id': request_id,
                    'sequence': request['sequence'], 'session_id': self.session_id, 'status': 'accepted', 'body': result}
        self.next_sequence += 1
        self.replay[request_id] = (encoded, copy.deepcopy(response))
        return response

    def record_answer_fixture(self, sdp):
        require(self.state == 'offer-set', 'InvalidState')
        validate_sdp(sdp)
        require(len(self.events) < MAX_EVENTS, 'ResourceLimit')
        self.events.append({'event_id': len(self.events) + 1, 'type': 'answer', 'body': {'revision': 1, 'sdp': sdp}})


class JanusTransaction:
    """Selected asynchronous plugin-request semantics, separate from the Direct draft."""
    def __init__(self, transaction, session_id, handle_id, plugin):
        self.transaction, self.session_id, self.handle_id, self.plugin = transaction, session_id, handle_id, plugin
        self.acknowledged = False
        self.result = None

    def consume(self, message):
        require(type(message) is dict)
        if self.result is not None or message.get('transaction') != self.transaction:
            return False
        if type(message.get('session_id')) is not int or message['session_id'] != self.session_id:
            return False
        kind = message.get('janus')
        if kind == 'ack':
            self.acknowledged = True
            return True
        if kind == 'error':
            require(type(message.get('error')) is dict and type(message['error'].get('code')) is int)
            self.result = copy.deepcopy(message)
            return True
        if kind not in ('event', 'success') or type(message.get('sender')) is not int or message['sender'] != self.handle_id:
            return False
        plugin_data = message.get('plugindata')
        if not isinstance(plugin_data, dict) or plugin_data.get('plugin') != self.plugin:
            return False
        require(type(plugin_data.get('data')) is dict)
        self.result = copy.deepcopy(message)
        return True
