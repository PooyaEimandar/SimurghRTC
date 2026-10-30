#!/usr/bin/env python3
"""Validate Direct v1 signaling between packaged Mac Unreal client and Server processes."""
from __future__ import annotations
import argparse
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import secrets
import socket
import subprocess
import threading
import time
import uuid
import mobile_artifacts as a
from run_signaling_probe import server_bundle_id
from run_mobile_probe import read_report

PATH = '/simurghrtc/v1/signal'
MAX_BODY = 256 * 1024


def validate_server(report, run_id, pid, manifest, application_messages=False, *, loopback=True):
    expected = dict(schema=1, run_id=run_id, pid=pid, server_build=True, editor_build=False, with_janus=False,
                    arm64_build=True, platform='Mac', artifact_manifest_sha256=manifest,
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d', loopback_only=loopback,
                    network_media_tested=False, application_messages_tested=application_messages, passed=True, error='',
                    callbacks_on_game_thread=True, running=False, live_peers=0, teardown_complete=True)
    a.require(type(report) is dict, 'Missing Server report.')
    for key,value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key] == value, 'Unexpected Server '+key)
    a.require(type(report.get('engine_version')) is str and report['engine_version'].startswith('5.8.3-'), 'Expected UE 5.8.3 Server.')
    for key in ('created','connected','closed','expired','replays','rejected','early_ice'):
        a.require(type(report.get(key)) is int and report[key] >= 0, 'Invalid Server counter: '+key)
    a.require(report['created'] == report['closed'], 'Server has unclosed sessions.')
    for key in ('connected','closed'):
        values=report.get(key+'_session_ids')
        a.require(type(values) is list and len(values)==report[key] and len(set(values))==len(values), 'Invalid Server session inventory.')
        for value in values: a.require(type(value) is str and str(uuid.UUID(value))==value, 'Invalid Server session UUID.')
    if report['connected']:
        transport=report.get('last_transport',{})
        a.require(transport.get('connection')==2 and transport.get('dtls')==3 and transport.get('sctp_open') is True
                  and type(transport.get('stun_responses')) is int and transport['stun_responses']>0, 'Missing Server transport evidence.')


def validate_client(report, run_id, pid, manifest, with_janus, pairs=4, scenario='connect', expected_error='', application_messages=False):
    a.require(isinstance(report, dict), 'Missing client report.')
    expected = dict(schema=1, run_id=run_id, pid=pid, server_build=False, editor_build=False,
                    arm64_build=True, platform='Mac', with_janus=with_janus,
                    artifact_manifest_sha256=manifest, native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',
                    loopback_only=True, network_media_tested=False, application_messages_tested=application_messages,
                    callbacks_on_game_thread=True, passed=True, error='', scenario=scenario,
                    expected_error=expected_error, requested_pairs=pairs, completed_pairs=pairs,
                    owner_callback_suppressed=True)
    for key, value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key] == value, 'Unexpected client ' + key)
    a.require(type(report.get('engine_version')) is str and report['engine_version'].startswith('5.8.3-'), 'Expected UE 5.8.3.')
    elapsed = report.get('elapsed_seconds')
    a.require(type(elapsed) in (float, int) and math.isfinite(elapsed) and 0 <= elapsed <= pairs * 30 + 20, 'Invalid client duration.')
    rows = report.get('pairs')
    a.require(type(rows) is list and len(rows) == pairs and all(type(r) is dict for r in rows), 'Missing client pair results.')
    identities = []
    for index, row in enumerate(rows):
        a.require(row.get('native_closed') is True, 'Native cleanup did not finish.')
        if expected_error:
            a.require(row.get('expected_error') == expected_error, 'Expected failure not observed.')
            continue
        a.require(row.get('remote_closed') is True, 'Remote cleanup was not acknowledged.')
        identity = row.get('session_id')
        a.require(type(identity) is str and str(uuid.UUID(identity)) == identity, 'Invalid session ID.')
        identities.append(identity)
        if scenario in ('cancel', 'owner-loss'):
            a.require(row.get('cancelled') is True, 'Expected cancellation not observed.')
            continue
        a.require(row.get('index') == index and row.get('mode') == ('media' if index % 2 else 'data')
                  and row.get('early_ice') is (index % 3 == 0), 'Wrong pair mode/order.')
        t = row.get('transport', {})
        a.require(type(t) is dict and all(type(t.get(k)) is int for k in ('connection','ice','dtls','protocol','queued_ice')),
                  'Transport states must be integers.')
        a.require(type(t) is dict and t.get('connection') == 2 and t.get('ice') in (2, 3) and t.get('dtls') == 3
                  and t.get('protocol') in (1, 2) and t.get('sctp_open') is True and t.get('remote_end') is True
                  and t.get('queued_ice') == 0, 'Transport did not connect.')
        for key in ('applied_ice', 'stats_samples', 'stun_responses'):
            a.require(type(t.get(key)) is int and t[key] > 0, 'Missing transport evidence: ' + key)
        a.require(type(row.get('requests')) is int and 5 <= row['requests'] <= 128, 'Invalid request count.')
        a.require(type(row.get('retries')) is int and 0 <= row['retries'] <= 128, 'Invalid retry count.')
    a.require(len(set(identities)) == len(identities), 'A session was reused across pairs.')


def request(port, token, payload=None, *, path=PATH, method='POST', raw=None, timeout=2):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    body = raw if raw is not None else (json.dumps(payload, separators=(',', ':')).encode() if payload is not None else None)
    headers = {'Content-Type': 'application/json'}
    if token is not None: headers['Authorization'] = 'Bearer ' + token
    try:
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse(); data = response.read(MAX_BODY + 1)
        a.require(len(data) <= MAX_BODY, 'Oversized HTTP result.')
        return response.status, json.loads(data) if data else None
    finally:
        connection.close()


def message(kind, sequence=0, session=None, body=None, request_id=None):
    result = dict(protocol='simurghrtc.direct', version=1, type=kind, request_id=request_id or uuid.uuid4().hex,
                  sequence=sequence, body=body or {})
    if session is not None: result['session_id'] = session
    return result


def wait_until(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value: return value
        time.sleep(0.1)
    raise ValueError('Timed out waiting for the expected probe state.')


class FaultProxy:
    """One-run loopback fixture: controlled loss/delay or a deliberately invalid HTTP response."""
    def __init__(self, target, kind):
        self.target, self.kind, self.injected = target, kind, False
        self.lock = threading.Lock()
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def do_POST(self):
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= MAX_BODY: self.send_error(400); return
                data = self.rfile.read(length)
                try: value = json.loads(data)
                except ValueError: self.send_error(400); return
                with owner.lock:
                    first = value.get('type') == 'create' and not owner.injected
                    if first: owner.injected = True
                if owner.kind == 'timeout': time.sleep(2); self.close_connection = True; return
                if owner.kind == 'oversize':
                    status, response = 200, b'x' * (MAX_BODY + 1)
                else:
                    conn = http.client.HTTPConnection('127.0.0.1', owner.target, timeout=3)
                    try:
                        conn.request('POST', PATH, data, {'Content-Type': 'application/json', 'Authorization': self.headers.get('Authorization', '')})
                        upstream = conn.getresponse(); status = upstream.status; response = upstream.read(MAX_BODY + 1)
                    finally: conn.close()
                    if first and owner.kind == 'drop':
                        self.close_connection = True
                        try: self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError: pass
                        return
                    if first and owner.kind == 'delay': time.sleep(0.9)
                    if owner.kind == 'correlation':
                        response = json.dumps(dict(json.loads(response), request_id='wrong-request')).encode()
                try:
                    self.send_response(status); self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(response))); self.end_headers(); self.wfile.write(response)
                except (BrokenPipeError, ConnectionResetError): pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
    def __enter__(self): self.thread.start(); return self
    def __exit__(self, *_): self.server.shutdown(); self.server.server_close(); self.thread.join(timeout=3)


def protocol_checks(port, token):
    checks = []
    def expect(label, packet, status=200, code=None, **kwargs):
        actual, result = request(port, token, packet, **kwargs)
        a.require(actual == status and isinstance(result, dict), 'Unexpected protocol result: ' + label)
        if code: a.require(result.get('status') == 'error' and result.get('body', {}).get('code') == code, 'Wrong protocol error: ' + label)
        else: a.require(result.get('status') == 'accepted', 'Unaccepted request: ' + label)
        checks.append(dict(case=label, status=actual, code=code, passed=True)); return result
    create = message('create', body=dict(purpose='data', identity='oracle'))
    expect('version', dict(create, version=2), 426, 'UnsupportedVersion')
    for label, raw in [('duplicate-key', json.dumps(create).replace('"sequence": 0', '"sequence": 0, "sequence": 0').encode()),
                       ('float-sequence', json.dumps(dict(create, sequence=0.0)).encode()),
                       ('invalid-utf8', b'{"protocol":"\xff"}')]:
        expect(label, None, 400, 'InvalidArgument', raw=raw)
    expect('body-limit', None, 413, 'PayloadTooLarge', raw=b' ' * (MAX_BODY + 1))
    status, result = request(port, None, create)
    a.require(status == 401 and result['body']['code'] == 'Unauthorized', 'Missing authorization was accepted.')
    checks.append(dict(case='authorization', passed=True))
    first = expect('create', create); session = first['session_id']
    a.require(str(uuid.UUID(session)) == session, 'Server must issue a UUID.')
    a.require(expect('create-replay', dict(reversed(list(create.items())))) == first, 'Replay changed its result.')
    expect('create-conflict', dict(create, body=dict(purpose='data', identity='changed')), 409, 'RequestConflict')
    poll = message('poll', 1, session, dict(after_event=0))
    status, result = request(port, token+'-other', poll)
    a.require(status == 404 and result['body']['code'] == 'SessionNotFound', 'Session authorization is not isolated.')
    checks.append(dict(case='principal-isolation', passed=True))
    expect('sequence-gap', dict(poll, sequence=2), 409, 'OutOfOrder')
    expect('future-cursor', dict(poll, body=dict(after_event=1)), 400, 'InvalidCursor')
    accepted = expect('poll', poll)
    a.require(expect('poll-replay', poll) == accepted, 'Poll replay did not preserve its snapshot.')
    expect('poll-conflict', dict(poll, body=dict(after_event=1)), 409, 'RequestConflict')
    expect('early-ice-end', message('ice',2,session,dict(revision=1,completed=True)))
    candidate=dict(revision=1,candidate='candidate:1 1 UDP 1 127.0.0.1 9 typ host',sdp_mid='0',sdp_mline_index=0,username_fragment='fixture')
    expect('candidate-after-end',message('ice',3,session,candidate),409,'IceComplete')
    expect('malformed-sdp-framing',message('offer',3,session,dict(revision=1,sdp='v=0\n')),400,'InvalidArgument')
    expect('native-sdp-validation',message('offer',3,session,dict(revision=1,sdp='v=0\r\n')))
    sequence=4
    while True:
        value=expect('native-error-poll',message('poll',sequence,session,dict(after_event=0)));sequence+=1
        if any(e.get('type')=='error' and e.get('body',{}).get('code')=='NativeError' for e in value['body']['events']):break
        a.require(sequence<24,'Native SDP rejection did not produce an error event.')
        time.sleep(0.1)
    a.require(expect('old-poll-snapshot-after-new-event',poll)==accepted,'Retried poll changed after new events arrived.')
    # Fill the real Server replay cache. No media is sent, and close must still work.
    for sequence in range(sequence, 129):
        status, _ = request(port, token, message('poll', sequence, session, dict(after_event=0)))
        a.require(status == 200, 'Replay cache filled before its documented capacity.')
    expect('replay-capacity', message('poll', 129, session, dict(after_event=0)), 503, 'ResourceLimit')
    close = message('close', 0, session)
    closed = expect('close-at-capacity-and-stale-sequence', close)
    a.require(expect('close-replay', close) == closed, 'Close retry changed its result.')
    expect('close-conflict', dict(close, sequence=1), 409, 'RequestConflict')
    expect('closed-session', message('poll', 129, session, dict(after_event=0)), 409, 'SessionClosed')
    orphan = expect('orphan-create', message('create', body=dict(purpose='data', identity='lease-test')))
    wait_until(lambda: request(port, token, path='/simurghrtc/probe/direct', method='GET')[1]['expired'] >= 1, 10)
    expect('lease-expiry', message('poll', 1, orphan['session_id'], dict(after_event=0)), 409, 'SessionClosed')
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server-project', 'client-project', 'server-executable', 'client-executable', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=4)
    args = parser.parse_args(); a.require(4 <= args.pairs <= 16, 'Use 4–16 connection pairs.')
    output = args.output.resolve(); a.require(not output.exists(), 'Use a new evidence directory.')
    server, client = args.server_executable.resolve(), args.client_executable.resolve()
    a.require(server.is_file() and client.is_file() and server != client, 'Provide distinct packaged Server and client executables.')
    sp, cp = args.server_project.resolve(), args.client_project.resolve()
    identity = a.validate(sp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    client_identity = a.validate(cp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(identity == client_identity, 'Client and Server must use the same validated Mac artifact.')
    server_profile = a.read_json(sp/'Plugins/SimurghRTC/SimurghRTC.profile.json')
    client_profile = a.read_json(cp/'Plugins/SimurghRTC/SimurghRTC.profile.json')
    a.require(server_profile['profile'] == 'server' and client_profile['with_direct'], 'Expected Server and Direct-enabled client profiles.')
    app_ids = [server_bundle_id(server), server_bundle_id(client)]
    for exe in (server, client):
        subprocess.run(['codesign','--verify','--deep','--strict',str(exe.parents[2])], check=True, capture_output=True)
        a.require(subprocess.check_output(['xcrun','lipo','-archs',str(exe)],text=True).strip() == 'arm64', 'Expected ARM64-only package.')
    output.mkdir(parents=True); run_id = str(uuid.uuid4()); token = secrets.token_urlsafe(32)
    record = dict(schema=1, passed=False, run_id=run_id, artifact=identity, profile=client_profile['profile'],
                  server_executable_sha256=a.digest(server), client_executable_sha256=a.digest(client), clients=[],
                  validation_scope='packaged Mac client/Server, separate processes, loopback HTTP and ICE; no application payloads or network media')
    process = None; client_processes = []; secret_files = []; user_dirs = {}
    def user_dir(app_id, case, secret=token):
        directory = Path.home()/'Library/Containers'/app_id/'Data/Library/Caches/SimurghRTC/DirectProbes'/run_id/case
        directory.mkdir(parents=True, exist_ok=False); path = directory/'token'
        with path.open('x') as stream: path.chmod(0o600); stream.write(secret)
        secret_files.append(path); return directory, path
    def launch(executable, app_id, case, extra, secret=token):
        directory, secret_path = user_dir(app_id, case, secret)
        case_id = str(uuid.uuid4()); user_dirs[case_id] = directory
        command = [str(executable),'-nullrhi','-nosound','-unattended','-stdout','-FullStdOutLogOutput',
                   '-UserDir='+str(directory),'-SimurghRTCDirectRunId='+case_id,'-SimurghRTCDirectTokenFile='+str(secret_path),*extra]
        target = output/case; target.mkdir()
        (target/'command.json').write_text(json.dumps(command,indent=2)+'\n')
        with (target/'process.log').open('w') as stream: child = subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
        return child,case_id,target
    def saved_report(case_id, target):
        paths = list(user_dirs[case_id].rglob('direct-probe-'+case_id+'.json'))
        a.require(len(paths)==1, 'Missing/ambiguous saved report for '+target.name)
        value = a.read_json(paths[0])
        logged = read_report((target/'process.log').read_text(errors='replace'),case_id,'SIMURGHRTC_DIRECT_REPORT ')
        a.require(logged == value, 'Saved and logged reports differ.')
        (target/'probe.json').write_text(json.dumps(value,indent=2)+'\n'); return value
    def run_client(case, port, pairs=1, scenario='connect', expected='', secret=token, retry=False):
        extra = ['-SimurghRTCDirectClientProbe','-SimurghRTCDirectEndpoint=http://127.0.0.1:'+str(port)+PATH,
                 '-SimurghRTCDirectPairs='+str(pairs),'-SimurghRTCDirectScenario='+scenario]
        if expected: extra += ['-SimurghRTCDirectExpectedError='+expected]
        child,cid,target=launch(client,app_ids[1],case,extra,secret);client_processes.append(child)
        result=dict(case=case,pid=child.pid,passed=False)
        record['clients'].append(result)
        result['exit_code']=child.wait(timeout=pairs*30+45)
        report=saved_report(cid,target);result['probe']=report
        validate_client(report,cid,child.pid,identity['manifest_sha256'],client_profile['with_janus'],pairs,scenario,expected)
        a.require(result['exit_code']==0 and child.pid != process.pid, 'Client/Server process identity or exit mismatch.')
        if retry: a.require(report['pairs'][0]['retries']>=1, 'The fault did not exercise HTTP retry.')
        result['passed']=True;result['app_stopped']=child.poll() is not None
        return report
    started=time.monotonic()
    try:
        with socket.socket() as sock: sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        process,sid,server_output=launch(server,app_ids[0],'server',['-SimurghRTCDirectServerProbe','-SimurghRTCDirectPort='+str(port)])
        record['server_pid']=process.pid
        def ready():
            a.require(process.poll() is None, 'Server exited before readiness.')
            try:
                status,health=request(port,token,path='/simurghrtc/probe/direct',method='GET',timeout=0.5)
                if status==200 and health.get('run_id')==sid and health.get('pid')==process.pid: return health
            except (OSError,http.client.HTTPException,ValueError): pass
            return None
        health=wait_until(ready,60)
        a.require(health['server_build'] is True and health['editor_build'] is False and health['artifact_manifest_sha256']==identity['manifest_sha256'], 'Wrong Server build/artifact.')
        record['protocol_checks']=protocol_checks(port,token)
        run_client('connections',port,args.pairs)
        run_client('unauthorized',port,expected='Unauthorized',secret=token+'-wrong')
        run_client('admission-rejected',port,scenario='denied',expected='AdmissionRejected')
        run_client('cancel',port,scenario='cancel')
        run_client('owner-loss',port,scenario='owner-loss')
        for fault,expected in [('drop',''),('delay',''),('correlation','InvalidResponse'),('oversize','PayloadTooLarge'),('timeout','TimedOut')]:
            with FaultProxy(port,fault) as proxy: run_client(fault,proxy.port,expected=expected,retry=fault in ('drop','delay'))
        wait_until(lambda: request(port,token,path='/simurghrtc/probe/direct',method='GET')[1]['live_peers']==0,15)
        status,_=request(port,token,path='/simurghrtc/probe/stop',payload={},method='POST')
        a.require(status==204,'Server stop was not accepted.')
        record['server_exit_code']=process.wait(timeout=20);report=saved_report(sid,server_output);record['server']=report
        validate_server(report,sid,process.pid,identity['manifest_sha256'])
        connected=[row['session_id'] for run in record['clients'] for row in run['probe']['pairs'] if 'transport' in row]
        a.require(len(connected)==args.pairs+2 and len(set(connected))==len(connected), 'Incomplete independent sessions.')
        a.require(set(report['connected_session_ids'])==set(connected) and report['connected']==len(connected), 'Client/Server connection evidence differs.')
        a.require(set(connected)<=set(report['closed_session_ids']) and report['created']==report['closed'], 'Server did not release all sessions.')
        a.require(report['replays']>=5 and report['expired']>=1 and report['early_ice']>=1 and record['server_exit_code']==0, 'Missing replay/expiry/early-ICE evidence.')
        with socket.socket() as sock: a.require(sock.connect_ex(('127.0.0.1',port))!=0,'Server listener remains open.')
        record['passed']=True
    except (ValueError,OSError,KeyError,subprocess.SubprocessError,http.client.HTTPException) as error:
        record['error']=str(error)
    finally:
        for child in client_processes+([process] if process else []):
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=10)
                except subprocess.TimeoutExpired: child.kill();child.wait(timeout=10)
        for path in secret_files: path.unlink(missing_ok=True)
        record['secrets_removed']=all(not p.exists() for p in secret_files)
        record['all_processes_stopped']=all(p.poll() is not None for p in client_processes+([process] if process else []))
        record['elapsed_seconds']=round(time.monotonic()-started,3)
        (output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(dict(passed=record['passed'],error=record.get('error'),evidence=str(output/'result.json'))))
    return 0 if record['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
