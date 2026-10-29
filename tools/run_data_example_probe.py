#!/usr/bin/env python3
"""Validate the actual packaged C++ and Blueprint data-client maps against Unreal Server."""
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
from run_direct_probe import PATH, FaultProxy, request, wait_until, validate_server
from run_mobile_probe import read_report
from run_signaling_probe import server_bundle_id

SCENARIOS = ('messages', 'timeout', 'disconnect-pending', 'destroy-connect', 'destroy-open')
MAPS = {'cpp': '/Game/Examples/Direct/Maps/RtcDataCpp', 'blueprint': '/Game/Examples/Direct/Maps/RtcDataBlueprint'}


def validate_example(report, run_id, pid, manifest, with_janus, implementation, scenario):
    a.require(implementation in MAPS and scenario in SCENARIOS, 'Unknown data-example case.')
    a.require(report.get('passed') is True, 'Data example failed: '+str(report.get('error', 'missing result')))
    expected = dict(schema=1, run_id=run_id, pid=pid, implementation=implementation, scenario=scenario,
                    map=MAPS[implementation], editor_build=False, server_build=False, arm64_build=True,
                    with_janus=with_janus, artifact_manifest_sha256=manifest,
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d', shared_api=True,
                    loopback_only=True, network_media_tested=False, error='', transports_created=1,
                    native_closed=1, pending_cleanup=0, objects_collected=True, end_play_calls=1,
                    end_play_closed=True, late_callbacks=0, callbacks_on_game_thread=True,
                    disconnect_requested=scenario=='disconnect-pending')
    for key, value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key]==value, 'Unexpected example report '+key)
    actor = '/Script/ContractHost.ContractDataCpp' if implementation=='cpp' else '/Game/Examples/Direct/BP_RtcData.BP_RtcData_C'
    a.require(report.get('actor_class')==actor, 'The packaged actor is not the intended implementation.')
    a.require(report.get('engine_version', '').startswith('5.8.3-'), 'Expected UE 5.8.3.')
    sid=report.get('session_id');a.require(type(sid) is str and str(uuid.UUID(sid))==sid and uuid.UUID(sid).int!=0, 'Invalid session ID.')
    for key in ('operations', 'completions', 'sent', 'received', 'channel_events', 'open_events', 'remote_closed', 'quiet_ticks', 'last_failure'):
        a.require(type(report.get(key)) is int and report[key]>=0, 'Invalid counter: '+key)
    a.require(report['quiet_ticks']>=8 and report['remote_closed']<=1, 'Missing quiet cleanup interval.')
    states=report.get('states')
    expected_states={'messages':[1,2,3,4], 'timeout':[1,5,3,4], 'disconnect-pending':[1,3,4], 'destroy-connect':[1], 'destroy-open':[1,2]}[scenario]
    a.require(type(states) is list and all(type(v) is int for v in states) and states==expected_states, 'Session state events differ.')
    pending=scenario.startswith('destroy-')
    a.require(report.get('pending_at_destruction') is pending and report.get('silent_cancelled') is pending, 'Pending owner cleanup differs.')
    outcomes = {
        'messages': ['Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
        'timeout': ['Connect:Failure','Disconnect:Success'],
        'disconnect-pending': ['Connect:Cancelled','Disconnect:Success'],
        'destroy-connect': [], 'destroy-open': ['Connect:Success'],
    }[scenario]
    a.require(report.get('outcomes')==outcomes and report['completions']==len(outcomes), 'Completion branches differ.')
    a.require(report['operations']==len(outcomes)+int(pending), 'Pending or duplicate operation count differs.')
    messages=scenario=='messages'
    a.require(report['sent']==report['received']==(2 if messages else 0), 'Message totals differ.')
    a.require(report.get('exact_payloads') is messages and report.get('channel_info_checked') is messages, 'Payload or channel verification missing.')
    a.require(report['last_failure']==(8 if scenario=='timeout' else 9 if scenario=='disconnect-pending' else 0), 'Structured error differs.')
    if messages:
        a.require(report['channel_events']>0 and report['open_events']>0 and report['remote_closed']==1, 'Channel events or acknowledged close missing.')
    return sid


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('server-project','client-project','server-executable','client-executable','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args=parser.parse_args();out=args.output.resolve();a.require(not out.exists(), 'Use a fresh evidence directory.')
    sp,cp=args.server_project.resolve(),args.client_project.resolve();server,client=args.server_executable.resolve(),args.client_executable.resolve()
    artifact=a.validate(sp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(artifact==a.validate(cp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64'), 'Native artifact mismatch.')
    profile=a.read_json(cp/'Plugins/SimurghRTC/SimurghRTC.profile.json')
    a.require(profile['with_direct'] and a.read_json(sp/'Plugins/SimurghRTC/SimurghRTC.profile.json')['profile']=='server', 'Expected Direct and Server profiles.')
    for exe in (server,client):
        subprocess.run(['codesign','--verify','--deep','--strict',str(exe.parents[2])], check=True, capture_output=True)
        a.require(subprocess.check_output(['xcrun','lipo','-archs',str(exe)],text=True).strip()=='arm64', 'Expected ARM64 package.')
    apps=[server_bundle_id(e) for e in (server,client)];out.mkdir(parents=True)
    run_id=str(uuid.uuid4());token=secrets.token_urlsafe(32);children={};secret_files=[]
    result=dict(schema=1,passed=False,run_id=run_id,artifact=artifact,profile=profile['profile'],clients={},
                client_executable_sha256=a.digest(client),server_executable_sha256=a.digest(server),
                scope='Mac ARM64 Development; actual cooked Blueprint/C++ maps, dedicated Server and loopback only')
    def launch(case, implementation=None, scenario=None, endpoint_port=None):
        is_server=implementation is None;app=apps[0 if is_server else 1]
        directory=Path.home()/'Library/Containers'/app/'Data/Library/Caches/SimurghRTC/DataExamples'/run_id/case
        directory.mkdir(parents=True,exist_ok=False);path=directory/'token';gate=directory/'trigger'
        with path.open('x') as stream:path.chmod(0o600);stream.write(token)
        secret_files.append(path);cid=str(uuid.uuid4());dest=out/case;dest.mkdir()
        command=[str(server if is_server else client)]
        if not is_server:command += [MAPS[implementation],'-game']
        command += ['-nullrhi','-nosound','-unattended','-stdout','-FullStdOutLogOutput','-UserDir='+str(directory)]
        if is_server:
            command += ['-SimurghRTCDirectServerProbe','-SimurghRTCDirectScenario=example','-SimurghRTCDirectRunId='+cid,
                        '-SimurghRTCDirectPort='+str(port),'-SimurghRTCDirectTokenFile='+str(path)]
        else:
            command += ['-SimurghRTCDataExampleProbe','-SimurghRTCSessionLoopbackProbe','-SimurghRTCExampleLoopback',
                        '-SimurghRTCExampleRunId='+cid,'-SimurghRTCExampleImplementation='+implementation,
                        '-SimurghRTCExampleScenario='+scenario,'-SimurghRTCExampleGate='+str(gate),
                        '-SimurghRTCExampleEndpoint=http://127.0.0.1:'+str(endpoint_port or port)+PATH,
                        '-SimurghRTCExampleTokenFile='+str(path)]
        (dest/'command.json').write_text(json.dumps(command,indent=2)+'\n')
        with (dest/'process.log').open('w') as stream:child=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
        children[case]=dict(process=child,run_id=cid,directory=directory,gate=gate,dest=dest,implementation=implementation,scenario=scenario)
    def read(case):
        c=children[case];server_case=case=='server';prefix='direct-probe-' if server_case else 'data-example-'
        paths=list(c['directory'].rglob(prefix+c['run_id']+'.json'));a.require(len(paths)==1, 'Missing saved report: '+case)
        report=a.read_json(paths[0]);marker='SIMURGHRTC_DIRECT_REPORT ' if server_case else 'SIMURGHRTC_DATA_EXAMPLE_REPORT '
        logged=read_report((c['dest']/'process.log').read_text(errors='replace'),c['run_id'],marker)
        a.require(logged==report, 'Saved/logged report mismatch: '+case)
        (c['dest']/'probe.json').write_text(json.dumps(report,indent=2)+'\n');return report
    def health():
        a.require(children['server']['process'].poll() is None, 'Server stopped early.')
        status,h=request(port,None,path='/simurghrtc/probe/direct',method='GET',timeout=2)
        a.require(status==200 and h['run_id']==children['server']['run_id'] and h['pid']==children['server']['process'].pid, 'Wrong Server health.')
        (out/'health-latest.json').write_text(json.dumps(h,indent=2)+'\n');return h
    def await_health(predicate, timeout=90):
        def poll():
            try:
                h=health();return h if predicate(h) else None
            except (OSError,http.client.HTTPException):return None
        return wait_until(poll,timeout)
    def finish(case):
        c=children[case];code=c['process'].wait(timeout=75);report=read(case)
        result['clients'][case]=dict(pid=c['process'].pid,exit_code=code,probe=report)
        identity=validate_example(report,c['run_id'],c['process'].pid,artifact['manifest_sha256'],profile['with_janus'],c['implementation'],c['scenario'])
        a.require(code==0, 'Data example exited unsuccessfully: '+case);return identity
    started=time.monotonic()
    try:
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        launch('server');await_health(lambda h:h['running'])
        identities=[]
        for implementation in MAPS:
            for scenario in SCENARIOS:
                case=implementation+'-'+scenario
                if scenario in ('timeout','disconnect-pending','destroy-connect'):
                    with FaultProxy(port,'timeout') as proxy:
                        launch(case,implementation,scenario,proxy.port)
                        if scenario!='timeout':
                            wait_until(lambda:proxy.injected,15);children[case]['gate'].touch()
                        identities.append(finish(case))
                        a.require(proxy.injected, 'The pending Connect did not reach the HTTP fixture.')
                        result['clients'][case]['http_pending_verified']=True
                else:
                    launch(case,implementation,scenario);identities.append(finish(case))
                await_health(lambda h:h['live_peers']==0,20)
        a.require(len(set(identities))==10, 'Sample runs reused session identities.')
        request(port,token,{},path='/simurghrtc/probe/stop')
        code=children['server']['process'].wait(timeout=30);report=read('server');result['server']=report;result['server_exit_code']=code
        validate_server(report,children['server']['run_id'],children['server']['process'].pid,artifact['manifest_sha256'],application_messages=True)
        a.require(code==0 and report['records']==0 and report['echo_pending']==0, 'Server cleanup incomplete.')
        a.require(report['created']==report['connected']==report['closed']==4 and report['data_sent']==report['data_received']==4, 'Server connection/message totals differ.')
        with socket.socket() as sock:a.require(sock.connect_ex(('127.0.0.1',port))!=0, 'Server listener remains open.')
        result['passed']=True
    except (ValueError,OSError,KeyError,subprocess.SubprocessError,http.client.HTTPException) as error:result['error']=str(error)
    finally:
        for c in children.values():
            child=c['process']
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=10)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=10)
        for path in secret_files:path.unlink(missing_ok=True)
        result['secrets_removed']=all(not p.exists() for p in secret_files)
        result['all_processes_stopped']=all(c['process'].poll() is not None for c in children.values())
        result['elapsed_seconds']=round(time.monotonic()-started,3)
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(passed=result['passed'],evidence=str(out/'result.json'),error=result.get('error'))))
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
