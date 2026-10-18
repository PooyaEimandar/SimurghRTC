#!/usr/bin/env python3
"""Live private-network Janus/TURN checks. Never emit authorization material."""
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request
import sys
import subprocess

from turn_probe import probe as probe_turn
from provenance import source_hashes

ROOT = Path(__file__).resolve().parent


def require(condition, message):
    if not condition:
        raise ValueError(message)


def probe_janus(secret, lock):
    base = 'http://janus:8088/janus'
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def request(path='', body=None, authenticated=True):
        url = base + path
        if body is not None:
            body = dict(body)
            if authenticated:
                body['apisecret'] = secret
            req = urllib.request.Request(url, json.dumps(body).encode(), {'Content-Type': 'application/json'})
        else:
            if authenticated:
                url += ('&' if '?' in url else '?') + urllib.parse.urlencode({'apisecret': secret})
            req = urllib.request.Request(url)
        with opener.open(req, timeout=35) as response:
            return json.load(response)
    deadline = time.monotonic() + 40
    while True:
        try:
            info = request('/info', authenticated=False)
            break
        except (OSError, urllib.error.URLError):
            if time.monotonic() >= deadline:
                raise TimeoutError('Janus readiness deadline')
            time.sleep(0.25)
    require(info['version_string'] == lock['janus']['tag'].removeprefix('v'), 'Janus version mismatch')
    plugins = sorted(info['plugins'])
    require(plugins == ['janus.plugin.' + name for name in lock['plugins']], 'Janus plugin selection mismatch')
    require(sorted(info['transports']) == ['janus.transport.' + name for name in lock['transports']], 'Janus transport selection mismatch')
    require(info.get('data_channels') is True or info.get('data_channels') == 'true', 'Janus DataChannels not compiled')
    unauthorized = request(body={'janus': 'create', 'transaction': 'sr005-unauthorized'}, authenticated=False)
    require(unauthorized.get('janus') == 'error' and unauthorized['error']['code'] == 403, 'Janus authentication not enforced')
    created = request(body={'janus': 'create', 'transaction': 'sr005-create'})
    require(created.get('janus') == 'success' and created['transaction'] == 'sr005-create', 'Janus create correlation failed')
    session = created['data']['id']
    require(type(session) is int and session > 0, 'Invalid Janus session ID')
    handle = None
    joined = False
    try:
        attached = request(f'/{session}', {'janus': 'attach', 'plugin': 'janus.plugin.videoroom', 'transaction': 'sr005-attach'})
        require(attached.get('janus') == 'success' and attached['transaction'] == 'sr005-attach', 'Janus attach failed')
        handle = attached['data']['id']
        listed = request(f'/{session}/{handle}', {'janus': 'message', 'transaction': 'sr005-list', 'body': {'request': 'list'}})
        require(listed.get('janus') == 'success' and listed['transaction'] == 'sr005-list', 'Room list correlation failed')
        rooms = listed['plugindata']['data']['list']
        require(any(room['room'] == lock['room_id'] for room in rooms), 'Pinned VideoRoom missing')
        ack = request(f'/{session}/{handle}', {'janus': 'message', 'transaction': 'sr005-join',
            'body': {'request': 'join', 'room': lock['room_id'], 'ptype': 'publisher', 'id': 42001, 'display': 'SR005 fixture'}})
        require(ack.get('janus') == 'ack' and ack['transaction'] == 'sr005-join', 'Expected asynchronous Janus ack')
        events = request(f'/{session}?maxev=5')
        events = events if isinstance(events, list) else [events]
        joined = any(e.get('janus') == 'event' and e.get('transaction') == 'sr005-join' and e.get('sender') == handle
                     and e.get('session_id') == session and e.get('plugindata', {}).get('plugin') == 'janus.plugin.videoroom'
                     and e['plugindata']['data'].get('videoroom') == 'joined' and e['plugindata']['data'].get('id') == 42001 for e in events)
        require(joined, 'Correlated VideoRoom joined event missing')
        trickle = request(f'/{session}/{handle}', {'janus': 'trickle', 'transaction': 'sr005-ice-end', 'candidate': {'completed': True}})
        require(trickle.get('janus') == 'ack' and trickle['transaction'] == 'sr005-ice-end', 'Trickle completion ack missing')
    finally:
        if handle is not None:
            detached = request(f'/{session}/{handle}', {'janus': 'detach', 'transaction': 'sr005-detach'})
            require(detached.get('janus') == 'success', 'Janus handle cleanup failed')
        destroyed = request(f'/{session}', {'janus': 'destroy', 'transaction': 'sr005-destroy'})
        require(destroyed.get('janus') == 'success', 'Janus session cleanup failed')
    stale = request(f'/{session}', {'janus': 'keepalive', 'transaction': 'sr005-stale'})
    require(stale.get('janus') == 'error' and stale['error']['code'] == 458, 'Destroyed session remained usable')
    return {'version': info['version_string'], 'plugins': plugins, 'unauthenticated_rejected': True,
            'session_created': True, 'handle_attached': True, 'configured_room_found': True,
            'ack_then_correlated_event': joined, 'trickle_completion_ack': True,
            'handle_detached': True, 'session_destroyed': True, 'stale_session_rejected': True,
            'peer_connection_media_tested': False}


def main():
    report = {'fixture_version': 'sr005-v1', 'passed': False, 'phase': 'janus'}
    try:
        lock = json.loads((ROOT / 'sources.lock.json').read_text())
        report['lock'] = lock
        report['service_sources'] = source_hashes(ROOT)
        report['janus'] = probe_janus(Path('/run/secrets/janus_api_secret').read_text().strip(), lock)
        report['phase'] = 'turn'
        version = subprocess.check_output(['/opt/coturn/bin/turnserver', '--version'], stderr=subprocess.STDOUT, text=True).strip()
        require(version == lock['coturn']['tag'], 'coturn version mismatch')
        report['turn'] = probe_turn(Path('/run/secrets/turn_shared_secret').read_text().strip())
        report['turn']['version'] = version
        report['passed'] = True
        report['phase'] = 'complete'
    except Exception as error:
        # urllib exceptions can contain credential-bearing URLs: never print exception text.
        report['error_type'] = type(error).__name__
        if isinstance(error, ValueError):
            report['error'] = str(error)  # Only constant assertions in these probes raise ValueError.
    print('SIMURGHRTC_SERVICE_REPORT ' + json.dumps(report, separators=(',', ':')))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
