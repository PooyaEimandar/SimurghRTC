#!/usr/bin/env python3
"""Validate SR-103 between packaged Mac ARM64 Unreal Game and dedicated Server."""
from __future__ import annotations
import argparse
import http.client
import json
from pathlib import Path
import secrets
import socket
import subprocess
import time
import uuid
import mobile_artifacts as a
from run_direct_probe import PATH, request, wait_until, validate_client, validate_server
from run_signaling_probe import server_bundle_id
from run_mobile_probe import read_report


def validate_data(report):
    for row in report['pairs']:
        data = row.get('data', {})
        expected = dict(phase=0, modes=6, closed_channels=6, messages=73, text_messages=18, binary_messages=55,
                        rejected_sends=25, ordered_burst=32, backpressure_observed=True, writable_resumed=True,
                        callbacks_on_game_thread=True, all_echoes_received=True)
        for key, value in expected.items():
            a.require(type(data.get(key)) is type(value) and data[key] == value, 'Invalid DataChannel evidence: '+key)


def validate_server_data(report, pairs):
    for key in ('data_channels_opened', 'data_channels_closed', 'data_received', 'data_sent', 'echo_pending'):
        a.require(type(report.get(key)) is int, 'Missing Server data counter: '+key)
    a.require(report['data_channels_opened'] == pairs * 6, 'Server channel open count differs.')
    # Each session closes six channels sequentially, then closes the peer. Peer close
    # suppresses callbacks immediately, so the last channel's queued close callback
    # may be cancelled. Client reports require all six Closed states; validate_server
    # separately requires all peer/session cleanup and zero remaining native peers.
    a.require(pairs * 5 <= report['data_channels_closed'] <= pairs * 6, 'Missing Server remote-close callbacks.')
    a.require(report['data_received'] == pairs * 73 and report['data_sent'] == pairs * 73 and report['echo_pending'] == 0,
              'Server echo counts differ.')


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
                  validation_scope='packaged Mac client/Server, separate processes, loopback HTTP and ICE; text/binary round trips, options, limits and backpressure; no network media or packet-loss test')
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
        validate_client(report,cid,child.pid,identity['manifest_sha256'],client_profile['with_janus'],pairs,scenario,expected,application_messages=True)
        validate_data(report)
        a.require(result['exit_code']==0 and child.pid != process.pid, 'Client/Server process identity or exit mismatch.')
        if retry: a.require(report['pairs'][0]['retries']>=1, 'The fault did not exercise HTTP retry.')
        result['passed']=True;result['app_stopped']=child.poll() is not None
        return report
    started=time.monotonic()
    try:
        with socket.socket() as sock: sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        process,sid,server_output=launch(server,app_ids[0],'server',['-SimurghRTCDirectServerProbe','-SimurghRTCDirectPort='+str(port),'-SimurghRTCDirectScenario=data'])
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
        run_client('data-round-trips',port,args.pairs,scenario='data')
        wait_until(lambda: request(port,token,path='/simurghrtc/probe/direct',method='GET')[1]['live_peers']==0,15)
        status,_=request(port,token,path='/simurghrtc/probe/stop',payload={},method='POST')
        a.require(status==204,'Server stop was not accepted.')
        record['server_exit_code']=process.wait(timeout=20);report=saved_report(sid,server_output);record['server']=report
        validate_server(report,sid,process.pid,identity['manifest_sha256'],application_messages=True)
        connected=[row['session_id'] for run in record['clients'] for row in run['probe']['pairs'] if 'transport' in row]
        a.require(len(connected)==args.pairs and len(set(connected))==len(connected), 'Incomplete independent sessions.')
        a.require(set(report['connected_session_ids'])==set(connected) and report['connected']==len(connected), 'Client/Server connection evidence differs.')
        a.require(set(connected)<=set(report['closed_session_ids']) and report['created']==report['closed'], 'Server did not release all sessions.')
        a.require(record['server_exit_code']==0, 'Server exit failed.')
        validate_server_data(report, args.pairs)
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
