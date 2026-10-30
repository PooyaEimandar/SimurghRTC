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
from network_validation import NETWORK_SCENARIOS, network_case, validate_network
from channel_validation import CHANNEL_SCENARIOS, channel_case, validate_channels

SCENARIOS = ('messages', 'timeout', 'disconnect-pending', 'destroy-connect', 'destroy-open')
BACKGROUND_SCENARIOS = ('background-connected', 'background-connect')
RECOVERY_SCENARIOS = ('reconnect', 'signaling-loss', 'relay-loss')
MAPS = {'cpp': '/Game/Examples/Direct/Maps/RtcDataCpp', 'blueprint': '/Game/Examples/Direct/Maps/RtcDataBlueprint'}


def validate_example(report, run_id, pid, manifest, with_janus, implementation, scenario, *, platform='Mac', simulator=False, loopback=True):
    recovery = scenario in RECOVERY_SCENARIOS; background = scenario in BACKGROUND_SCENARIOS
    channels = scenario in CHANNEL_SCENARIOS
    network = scenario in NETWORK_SCENARIOS
    basic_scenario = 'messages' if channels or network else scenario
    rounds = 2 if recovery or scenario=='background-connected' else 1
    connections = 2 if recovery or background else 1
    a.require(implementation in MAPS and scenario in (*SCENARIOS, *RECOVERY_SCENARIOS, *BACKGROUND_SCENARIOS, *CHANNEL_SCENARIOS, *NETWORK_SCENARIOS, 'unauthorized', 'tls-rejected'), 'Unknown data-example case.')
    a.require(report.get('passed') is True, 'Data example failed: '+str(report.get('error', 'missing result')))
    expected = dict(schema=1, run_id=run_id, pid=pid, implementation=implementation, scenario=scenario,
                    map=MAPS[implementation], editor_build=False, server_build=False, arm64_build=True,
                    with_janus=with_janus, artifact_manifest_sha256=manifest,
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d', shared_api=True,
                    platform=platform, simulator_build=simulator, loopback_only=loopback,
                    network_media_tested=False, error='', transports_created=connections,
                    native_closed=connections, pending_cleanup=0, objects_collected=True, end_play_calls=1,
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
    a.require(report['quiet_ticks']>=8 and report['remote_closed']<=connections, 'Missing quiet cleanup interval.')
    states=report.get('states')
    expected_states={'background-connected':[1,2,1,2,3,4], 'background-connect':[1,1,2,3,4], 'reconnect':[1,2,3,4,1,2,3,4], 'signaling-loss':[1,2,5,1,2,3,4], 'relay-loss':[1,2,5,1,2,3,4], 'messages':[1,2,3,4], 'timeout':[1,5,3,4], 'unauthorized':[1,5,3,4], 'tls-rejected':[1,5,3,4], 'disconnect-pending':[1,3,4], 'destroy-connect':[1], 'destroy-open':[1,2]}[basic_scenario]
    a.require(type(states) is list and all(type(v) is int for v in states) and states==expected_states, 'Session state events differ.')
    pending=scenario.startswith('destroy-')
    a.require(report.get('pending_at_destruction') is pending and report.get('silent_cancelled') is pending, 'Pending owner cleanup differs.')
    outcomes = {
        'background-connected': ['Connect:Success','OpenChannel:Success','Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
        'background-connect': ['Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
        'reconnect': ['Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success']*2,
        'signaling-loss': ['Connect:Success','OpenChannel:Success','Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
        'relay-loss': ['Connect:Success','OpenChannel:Success','Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
        'messages': ['Connect:Success','OpenChannel:Success','CloseChannel:Success','Disconnect:Success'],
        'timeout': ['Connect:Failure','Disconnect:Success'],
        'unauthorized': ['Connect:Failure','Disconnect:Success'], 'tls-rejected': ['Connect:Failure','Disconnect:Success'],
        'disconnect-pending': ['Connect:Cancelled','Disconnect:Success'],
        'destroy-connect': [], 'destroy-open': ['Connect:Success'],
    }[basic_scenario]
    if network:
        count=network_case(scenario)['channels']
        outcomes=['Connect:Success']+['OpenChannel:Success']*count+['CloseChannel:Success']*count+['Disconnect:Success']
    a.require(report.get('outcomes')==outcomes and report['completions']==len(outcomes), 'Completion branches differ.')
    a.require(report['operations']==len(outcomes)+int(pending or scenario=='background-connect'), 'Pending or duplicate operation count differs.')
    messages=basic_scenario=='messages' or recovery or background
    totals=network_case(scenario) if network else dict(sent=channel_case(scenario)['messages'] if channels else 2*rounds if messages else 0)
    a.require(report['sent']==totals['sent'] and report['received']==totals.get('received',totals['sent']), 'Message totals differ.')
    a.require(report.get('exact_payloads') is messages and report.get('channel_info_checked') is messages, 'Payload or channel verification missing.')
    a.require(report['last_failure']==(8 if scenario=='timeout' else 7 if scenario=='unauthorized' else 11 if scenario=='tls-rejected' else 9 if scenario=='disconnect-pending' else 0), 'Structured error differs.')
    if messages:
        a.require(report['channel_events']>0 and report['open_events']>0 and report['remote_closed']>=1, 'Channel events or acknowledged close missing.')
    if channels:validate_channels(report,scenario)
    if network:validate_network(report,scenario)
    if background:
        expected_background=dict(inactive_events=1,active_events=1,background_events=1,foreground_events=1,background_shutdown=True,
            background_pending=scenario=='background-connect',background_cancelled=scenario=='background-connect',
            retired_objects_collected=True,rounds_completed=rounds,stale_handle_checks=6 if rounds==2 else 0,recovery_requested=False)
        for key,value in expected_background.items():
            a.require(type(report.get(key)) is type(value) and report[key]==value,'Unexpected background result: '+key)
        ids=report.get('session_ids',[])
        a.require(type(ids) is list and len(ids)==2 and len(set(ids))==2 and ids[-1]==sid
                  and all(str(uuid.UUID(v))==v and uuid.UUID(v).int for v in ids),'Resume must create a fresh session.')
        channels=report.get('channel_ids',[])
        a.require(type(channels) is list and len(channels)==rounds and len(set(channels))==rounds,'Resume reused a channel.')
        a.require(report.get('connection_errors')==[],'Backgrounding should shut down without a connection failure.')
        elapsed=report.get('background_seconds');shutdown=report.get('shutdown_seconds')
        a.require(type(elapsed) in (int,float) and 3<=elapsed<60,'Missing bounded real background interval.')
        a.require(type(shutdown) in (int,float) and 0<=shutdown<1,'Background shutdown blocked the game thread.')
        gap=report.get('max_tick_gap_seconds');ticks=report.get('heartbeats')
        a.require(type(gap) in (int,float) and 0<=gap<2 and type(ticks) is int and ticks>=10,'Foreground game thread did not remain responsive.')
    if recovery:
        a.require(report.get('rounds_completed')==2 and report.get('stale_handle_checks')==6, 'Missing recovery/old-handle checks.')
        ids=report.get('channel_ids',[])
        a.require(len(ids)==2 and len(set(ids))==2 and all(str(uuid.UUID(v))==v and uuid.UUID(v).int for v in ids),'Reconnect reused/invalid channel identity.')
        a.require(report.get('recovery_requested') is (scenario!='reconnect'), 'Recovery gate differs.')
        errors=report.get('connection_errors')
        if scenario=='reconnect':a.require(errors==[] and report['remote_closed']==2,'Graceful reconnect lost a connection or cleanup acknowledgement.')
        else:a.require(type(errors) is list and len(errors)==1 and type(errors[0]) is int
                       and errors[0] in ([11] if scenario=='signaling-loss' else [2,11]),'Missing structured connection-loss error.')
        gap=report.get('max_tick_gap_seconds');ticks=report.get('heartbeats')
        a.require(type(gap) in (int,float) and 0<=gap<2 and type(ticks) is int and ticks>=10,'Game thread did not remain responsive.')
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
