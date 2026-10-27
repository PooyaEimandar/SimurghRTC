#!/usr/bin/env python3
"""Exercise the shared Core API with concurrent packaged Unreal clients and a dedicated Server."""
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
from run_signaling_probe import server_bundle_id
from run_mobile_probe import read_report

LIFECYCLES = ('cancel','disconnect-pending','timeout','http-timeout','reentrant','owner-loss','world','session-gc')


def validate_session(report, run_id, pid, manifest, with_janus, scenario, role=0, ready=False):
    a.require(scenario=='data' or scenario in LIFECYCLES,'Unknown shared probe scenario.')
    a.require(report.get('passed') is True,'Shared probe failed: '+str(report.get('error','missing result')))
    expected = dict(schema=1,run_id=run_id,pid=pid,scenario=scenario,role=role,editor_build=False,server_build=False,
                    arm64_build=True,platform='Mac',with_janus=with_janus,artifact_manifest_sha256=manifest,
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',shared_api=True,
                    loopback_only=True,network_media_tested=False,passed=True,error='',ready=ready)
    for key,value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key]==value,'Unexpected shared report '+key)
    a.require(report.get('engine_version','').startswith('5.8.3-'),'Expected UE 5.8.3.')
    for key in ('phase','quiet_ticks','pending_cleanup','transports_created','native_closed','remote_closed','isolation_checks'):
        a.require(type(report.get(key)) is int and report[key]>=0,'Invalid shared counter: '+key)
    rows=report.get('clients');a.require(type(rows) is list and len(rows)==(2 if scenario=='data' else 1),'Missing session inventory.')
    identities=[]
    for row in rows:
        for key in ('received','unexpected','operations','completions','last_error'):
            a.require(type(row.get(key)) is int and row[key]>=0,'Invalid endpoint counter: '+key)
        sid=row.get('facade_id');a.require(type(sid) is str and str(uuid.UUID(sid))==sid,'Invalid facade ID.');identities.append(sid)
        a.require(row.get('callbacks_on_game_thread') is True and row.get('unexpected')==0 and row.get('expected_empty') is True,
                  'Unexpected callback or incomplete message delivery.')
    a.require(len(set(identities))==len(identities),'Sessions reused facade identity.')
    if ready:
        a.require(report.get('phase')==2 and report.get('transports_created')==2 and report.get('native_closed')==0,'Invalid simultaneous readiness.')
        a.require(all(row.get('operations')==row.get('completions')==2 and row.get('received')==0 for row in rows),'Readiness lacks connected open channels.')
        return identities
    count=3 if scenario=='data' else 1
    a.require(report.get('phase')==100 and report.get('quiet_ticks',0)>=8 and report.get('pending_cleanup')==0
              and report.get('transports_created')==report.get('native_closed')==count,'Native cleanup or quiet period incomplete.')
    if scenario=='data':
        pressure=role==0
        a.require(report.get('isolation_checks')==6 and report.get('remote_closed')==3,'Session isolation or acknowledged close missing.')
        a.require(all(report.get(key) is pressure for key in ('backpressure_tested','backpressure_observed','backpressure_recovered')),'Backpressure evidence differs.')
        for row,received,operations in zip(rows,(9 if pressure else 4,2),(8,4)):
            a.require(row.get('received')==received and row.get('operations')==row.get('completions')==operations and row.get('last_error')==0,
                      'Message count or successful operation completion differs.')
    else:
        row=rows[0];silent=scenario in ('owner-loss','world','session-gc')
        operations=2 if silent or scenario=='disconnect-pending' else 1
        completions=1 if silent else operations
        code=9 if silent or scenario=='cancel' else 8 if scenario in ('timeout','http-timeout') else 0
        a.require(row.get('operations')==operations and row.get('completions')==completions and row.get('last_error')==code
                  and row.get('received')==0,'Lifecycle outcome differs.')
        if silent or scenario=='reentrant':a.require(row.get('triggered') is True,'Teardown trigger did not run.')
        if scenario=='session-gc':a.require(row.get('session_collected') is True,'Session was retained after GC.')
    return identities


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('server-project','client-project','server-executable','client-executable','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();a.require(not out.exists(),'Use a fresh evidence directory.')
    sp,cp=args.server_project.resolve(),args.client_project.resolve();server,client=args.server_executable.resolve(),args.client_executable.resolve()
    artifact=a.validate(sp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(artifact==a.validate(cp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64'),'Native artifact mismatch.')
    profile=a.read_json(cp/'Plugins/SimurghRTC/SimurghRTC.profile.json')
    a.require(profile['with_direct'] and a.read_json(sp/'Plugins/SimurghRTC/SimurghRTC.profile.json')['profile']=='server','Expected Direct and Server profiles.')
    for exe in (server,client):
        subprocess.run(['codesign','--verify','--deep','--strict',str(exe.parents[2])],check=True,capture_output=True)
        a.require(subprocess.check_output(['xcrun','lipo','-archs',str(exe)],text=True).strip()=='arm64','Expected ARM64 package.')
    apps=[server_bundle_id(e) for e in (server,client)];out.mkdir(parents=True)
    run_id=str(uuid.uuid4());token=secrets.token_urlsafe(32);children={};secret_files=[]
    result=dict(schema=1,passed=False,run_id=run_id,artifact=artifact,profile=profile['profile'],clients={},
                client_executable_sha256=a.digest(client),server_executable_sha256=a.digest(server),
                scope='Mac ARM64 shared API, four packaged Game processes with two sessions each, dedicated Server, loopback only')
    def launch(case,scenario=None,role=0,endpoint_port=None):
        is_server=scenario is None;app=apps[0 if is_server else 1]
        directory=Path.home()/'Library/Containers'/app/'Data/Library/Caches/SimurghRTC/SessionProbes'/run_id/case
        directory.mkdir(parents=True,exist_ok=False);path=directory/'token';gate=directory/'go'
        with path.open('x') as stream:path.chmod(0o600);stream.write(token)
        secret_files.append(path);cid=str(uuid.uuid4());dest=out/case;dest.mkdir()
        command=[str(server if is_server else client),'-nullrhi','-nosound','-unattended','-stdout','-FullStdOutLogOutput','-UserDir='+str(directory)]
        if is_server:
            command+=['-SimurghRTCDirectServerProbe','-SimurghRTCDirectScenario=shared','-SimurghRTCDirectRunId='+cid,
                      '-SimurghRTCDirectPort='+str(port),'-SimurghRTCDirectTokenFile='+str(path)]
        else:
            command+=['-SimurghRTCSharedSessionProbe','-SimurghRTCSessionLoopbackProbe','-SimurghRTCSessionScenario='+scenario,
                      '-SimurghRTCSessionRunId='+cid,'-SimurghRTCSessionEndpoint=http://127.0.0.1:'+str(endpoint_port or port)+PATH,
                      '-SimurghRTCSessionTokenFile='+str(path),'-SimurghRTCSessionGate='+str(gate),'-SimurghRTCSessionRole='+str(role)]
            if scenario=='data' and role==0:command+=['-SimurghRTCSessionPressure']
        (dest/'command.json').write_text(json.dumps(command,indent=2)+'\n')
        with (dest/'process.log').open('w') as stream:child=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
        children[case]=dict(process=child,run_id=cid,directory=directory,gate=gate,dest=dest,scenario=scenario,role=role)
    def read(case,ready=False):
        c=children[case];server_case=case=='server'
        prefix='direct-probe-' if server_case else 'session-ready-' if ready else 'session-probe-'
        paths=list(c['directory'].rglob(prefix+c['run_id']+'.json'));a.require(len(paths)==1,'Missing saved report: '+case)
        report=a.read_json(paths[0]);marker='SIMURGHRTC_DIRECT_REPORT ' if server_case else 'SIMURGHRTC_SESSION_READY ' if ready else 'SIMURGHRTC_SESSION_REPORT '
        logged=read_report((c['dest']/'process.log').read_text(errors='replace'),c['run_id'],marker)
        a.require(logged==report,'Saved/logged report mismatch: '+case)
        (c['dest']/('ready.json' if ready else 'probe.json')).write_text(json.dumps(report,indent=2)+'\n');return report
    def health():
        a.require(children['server']['process'].poll() is None,'Server stopped early.')
        status,h=request(port,None,path='/simurghrtc/probe/direct',method='GET',timeout=2)
        a.require(status==200 and h['run_id']==children['server']['run_id'] and h['pid']==children['server']['process'].pid,'Wrong Server health.')
        (out/'health-latest.json').write_text(json.dumps(h,indent=2)+'\n');return h
    def await_health(predicate,timeout=90):
        def poll():
            try:
                h=health();return h if predicate(h) else None
            except (OSError,http.client.HTTPException):return None
        return wait_until(poll,timeout)
    def finish(case):
        c=children[case];code=c['process'].wait(timeout=110);report=read(case)
        ids=validate_session(report,c['run_id'],c['process'].pid,artifact['manifest_sha256'],profile['with_janus'],c['scenario'],c['role'])
        a.require(code==0,'Shared client process failed: '+case)
        result['clients'][case]=dict(pid=c['process'].pid,exit_code=code,probe=report);return ids
    started=time.monotonic()
    try:
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        launch('server');await_health(lambda h:h['running'])
        for role in range(4):launch('data-'+str(role),'data',role)
        await_health(lambda h:h['connected_sessions']==8 and h['live_peers']==8)
        ids=[]
        for role in range(4):
            case='data-'+str(role);c=children[case]
            wait_until(lambda:list(c['directory'].rglob('session-ready-'+c['run_id']+'.json')),30)
            a.require(c['process'].poll() is None,'Clients did not overlap.')
            r=read(case,True);ids+=validate_session(r,c['run_id'],c['process'].pid,artifact['manifest_sha256'],profile['with_janus'],'data',role,True)
        a.require(len(set(ids))==8,'Facade identities were shared across processes.')
        result['overlap_pids']=[children['data-'+str(i)]['process'].pid for i in range(4)]
        a.require(len(set(result['overlap_pids']))==4,'Expected four separate client processes.')
        for role in range(4):children['data-'+str(role)]['gate'].touch()
        for role in range(4):finish('data-'+str(role))
        h=await_health(lambda h:h['live_peers']==0,20)
        a.require(h['connected']==12 and h['created']==h['closed']==12 and h['data_sent']==h['data_received']==29,'Concurrent data totals differ.')
        result['concurrent_server']=h
        for scenario in LIFECYCLES:
            if scenario in ('timeout','http-timeout'):
                with FaultProxy(port,'timeout') as proxy:
                    launch(scenario,scenario,endpoint_port=proxy.port);finish(scenario)
                    a.require(proxy.injected,'Timeout did not reach the HTTP fixture.')
                result[scenario.replace('-','_')+'_injected']=True
            else:launch(scenario,scenario);finish(scenario)
            await_health(lambda h:h['live_peers']==0,20)
        request(port,token,{},path='/simurghrtc/probe/stop')
        code=children['server']['process'].wait(timeout=30);report=read('server');result['server']=report;result['server_exit_code']=code
        validate_server(report,children['server']['run_id'],children['server']['process'].pid,artifact['manifest_sha256'],application_messages=True)
        a.require(code==0 and report['records']==0 and report['echo_pending']==0 and report['peak_connected_sessions']==8,'Server did not clean up concurrent sessions.')
        a.require(report['connected']==16 and 16<=report['created']<=18 and report['data_sent']==report['data_received']==29,'Lifecycle/echo totals differ.')
        with socket.socket() as sock:a.require(sock.connect_ex(('127.0.0.1',port))!=0,'Listener remains open.')
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
    print(json.dumps(dict(passed=result['passed'],error=result.get('error'),evidence=str(out/'result.json'))))
    return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
