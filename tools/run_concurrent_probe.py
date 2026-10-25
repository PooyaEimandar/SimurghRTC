#!/usr/bin/env python3
"""Validate simultaneous packaged Unreal clients, addressed routing and session cleanup."""
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
from run_direct_probe import PATH, request, message, wait_until, validate_server
from run_signaling_probe import server_bundle_id
from run_mobile_probe import read_report

ROLES = ('alpha', 'beta', 'gamma', 'delta')
EXPECTED_RECEIVED = {1: dict(alpha=7, beta=7, gamma=4, delta=6), 2: dict(alpha=5, beta=6, gamma=4)}


def validate_client(report, run_id, pid, manifest, with_janus, role, replacement=False, progress=False):
    expected = dict(schema=1, run_id=run_id, pid=pid, server_build=False, editor_build=False, arm64_build=True,
                    platform='Mac', with_janus=with_janus, artifact_manifest_sha256=manifest,
                    native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d', loopback_only=True,
                    network_media_tested=False, application_messages_tested=True, passed=True)
    if not progress: expected['scenario'] = 'concurrent'
    for key, value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key] == value, 'Unexpected client '+key)
    a.require(report.get('engine_version','').startswith('5.8.3-'), 'Expected Unreal 5.8.3.')
    data = report.get('concurrent', {})
    for key, value in dict(role=role, replacement=replacement, callbacks_on_game_thread=True, unexpected_messages=False).items():
        a.require(type(data.get(key)) is type(value) and data[key] == value, 'Unexpected concurrent '+key)
    sid = data.get('session_id'); a.require(type(sid) is str and str(uuid.UUID(sid))==sid, 'Invalid session ID.')
    phases = [1] if progress or (role=='beta' and not replacement) else [2] if replacement else [1,2]
    rows = data.get('phases'); a.require(type(rows) is list and len(rows)==len(phases), 'Wrong phase inventory.')
    for phase,row in zip(phases,rows):
        expected = dict(phase=phase, received=EXPECTED_RECEIVED[phase][role], expected=EXPECTED_RECEIVED[phase][role], sealed=True)
        for key,value in expected.items():
            a.require(type(row.get(key)) is type(value) and row[key]==value, 'Missing exact delivery/barrier: '+key)
    if not progress:
        a.require(report.get('error')=='' and report.get('native_closed') is True and report.get('callbacks_on_game_thread') is True,
                  'Client error or incomplete cleanup.')
        server_closed = role=='beta' and not replacement
        a.require(report.get('remote_closed') is (not server_closed), 'Wrong close acknowledgment.')
        a.require(report.get('close_reason') == ('SessionClosed' if server_closed else ''), 'Wrong close reason.')
    return sid


def validate_concurrent_server(report, identities):
    a.require(set(identities)=={'alpha','beta','gamma','delta','beta-rejoin'}, 'Missing client identity inventory.')
    for key,value in dict(created=5,connected=5,closed=5,expired=1,live_peers=0,records=0,connected_sessions=0,
                          peak_live_peers=4,peak_connected_sessions=4).items():
        a.require(type(report.get(key)) is int and report[key]==value, 'Unexpected Server '+key)
    data=report.get('concurrent',{})
    for key,value in dict(ready=0,phase=2,sealed=3,native_closed=5,isolation_checks=9,callbacks_on_game_thread=True).items():
        a.require(type(data.get(key)) is type(value) and data[key]==value, 'Missing Server concurrent '+key)
    a.require(len(set(identities.values()))==5, 'A session was reused.')
    for key in ('connected_session_ids','closed_session_ids'):
        a.require(set(report.get(key,[]))==set(identities.values()), 'Server lifecycle IDs differ from client sessions.')
    sessions=data.get('sessions');a.require(type(sessions) is list and len(sessions)==5, 'Missing lifecycle inventory.')
    expected={sid:case.split('-')[0] for case,sid in identities.items()}
    a.require({row.get('session_id'):row.get('principal') for row in sessions}==expected, 'Authenticated session ownership differs.')
    a.require(all(type(row.get('state')) is int and row['state']==3 and row.get('native_closed') is True for row in sessions), 'Not every session reached native Closed.')
    rows=data.get('phases');a.require(type(rows) is list and len(rows)==2, 'Missing concurrent phases.')
    for row,phase,clients,echoes,routed,denied in zip(rows,(1,2),(4,3),(8,6),(8,3),(8,6)):
        for key,value in dict(phase=phase,clients=clients,echoes=echoes,routed=routed,denied=denied).items():
            a.require(type(row.get(key)) is int and row[key]==value, 'Wrong routing count: '+key)
        wanted={identities[k] for k in (('alpha','beta','gamma','delta') if phase==1 else ('alpha','beta-rejoin','gamma'))}
        values=row.get('sealed_sessions',[])
        a.require(len(values)==clients and set(values)==wanted, 'Phase did not include the expected simultaneous sessions.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('server-project','client-project','server-executable','client-executable','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();a.require(not out.exists(),'Use a fresh evidence directory.')
    server,client=args.server_executable.resolve(),args.client_executable.resolve()
    sp,cp=args.server_project.resolve(),args.client_project.resolve()
    artifact=a.validate(sp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(artifact==a.validate(cp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64'),'Artifact mismatch.')
    profiles=[a.read_json(p/'Plugins/SimurghRTC/SimurghRTC.profile.json') for p in (sp,cp)]
    a.require(profiles[0]['profile']=='server' and profiles[1]['with_direct'],'Expected Server and Direct-enabled client.')
    for exe in (server,client):
        subprocess.run(['codesign','--verify','--deep','--strict',str(exe.parents[2])],check=True,capture_output=True)
        a.require(subprocess.check_output(['xcrun','lipo','-archs',str(exe)],text=True).strip()=='arm64','Expected ARM64 package.')
    app_ids=[server_bundle_id(e) for e in (server,client)];out.mkdir(parents=True)
    run_id=str(uuid.uuid4());admin=secrets.token_urlsafe(32);tokens={r:secrets.token_urlsafe(32) for r in (*ROLES,'spare')}
    children={};secrets_paths=[];record=dict(schema=1,passed=False,run_id=run_id,artifact=artifact,
        server_executable_sha256=a.digest(server),client_executable_sha256=a.digest(client),profile=profiles[1]['profile'],clients={},
        scope='Mac ARM64 packaged Server and simultaneous Game processes, loopback only; no network media or mobile qualification')
    def launch(case,role=None,replacement=False):
        is_server=role is None;app=app_ids[0 if is_server else 1]
        directory=Path.home()/'Library/Containers'/app/'Data/Library/Caches/SimurghRTC/ConcurrentProbes'/run_id/case
        directory.mkdir(parents=True,exist_ok=False);secret=directory/'token'
        with secret.open('x') as f:secret.chmod(0o600);f.write(admin if is_server else tokens[role])
        secrets_paths.append(secret);cid=str(uuid.uuid4());dest=out/case;dest.mkdir()
        command=[str(server if is_server else client),'-nullrhi','-nosound','-unattended','-stdout','-FullStdOutLogOutput',
                 '-UserDir='+str(directory),'-SimurghRTCDirectScenario=concurrent','-SimurghRTCDirectRunId='+cid,'-SimurghRTCDirectTokenFile='+str(secret)]
        if is_server:
            credentials=directory/'credentials.json'
            with credentials.open('x') as f:credentials.chmod(0o600);json.dump(tokens,f)
            secrets_paths.append(credentials)
            command+=['-SimurghRTCDirectServerProbe','-SimurghRTCDirectPort='+str(port),'-SimurghRTCConcurrentCredentials='+str(credentials)]
        else:
            command+=['-SimurghRTCDirectClientProbe','-SimurghRTCDirectEndpoint=http://127.0.0.1:'+str(port)+PATH,'-SimurghRTCConcurrentRole='+str(ROLES.index(role))]
            if replacement:command+=['-SimurghRTCConcurrentReplacement']
        (dest/'command.json').write_text(json.dumps(command,indent=2)+'\n')
        with (dest/'process.log').open('w') as f:child=subprocess.Popen(command,stdout=f,stderr=subprocess.STDOUT)
        children[case]=dict(process=child,run_id=cid,directory=directory,output=dest,role=role,replacement=replacement)
    def read(case,progress=False):
        c=children[case];prefix='concurrent-progress-' if progress else 'direct-probe-'
        paths=list(c['directory'].rglob(prefix+c['run_id']+'.json'));a.require(len(paths)==1,'Missing report: '+case)
        value=a.read_json(paths[0]);marker='SIMURGHRTC_CONCURRENT_PROGRESS ' if progress else 'SIMURGHRTC_DIRECT_REPORT '
        logged=read_report((c['output']/'process.log').read_text(errors='replace'),c['run_id'],marker)
        a.require(value==logged,'Saved/logged report mismatch: '+case)
        (c['output']/('progress.json' if progress else 'probe.json')).write_text(json.dumps(value,indent=2)+'\n');return value
    def health():
        a.require(children['server']['process'].poll() is None,'Server stopped early.')
        status,h=request(port,None,path='/simurghrtc/probe/direct',method='GET',timeout=3)
        a.require(status==200 and h['run_id']==children['server']['run_id'] and h['pid']==children['server']['process'].pid,'Wrong Server health.')
        (out/'health-latest.json').write_text(json.dumps(h,indent=2)+'\n')
        return h
    def await_health(predicate,seconds=90):
        def poll():
            try:
                h=health();return h if predicate(h) else None
            except (OSError,http.client.HTTPException):return None
        return wait_until(poll,seconds)
    def control(action):
        status,_=request(port,admin,dict(action=action),path='/simurghrtc/probe/concurrent')
        a.require(status==204,'Control rejected: '+action)
    def finish_client(case):
        c=children[case];code=c['process'].wait(timeout=45);r=read(case)
        sid=validate_client(r,c['run_id'],c['process'].pid,artifact['manifest_sha256'],profiles[1]['with_janus'],c['role'],c['replacement'])
        a.require(code==0,'Client process failed: '+case)
        record['clients'][case]=dict(pid=c['process'].pid,exit_code=code,probe=r);return sid
    start=time.monotonic()
    try:
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        launch('server');await_health(lambda h:h['running'],90)
        for role in ROLES[:3]:launch(role,role)
        h=await_health(lambda h:h['concurrent']['ready']==3 and h['connected_sessions']==3)
        record['principal_capacity_spare_global_slot']=h['live_peers']==3
        a.require(record['principal_capacity_spare_global_slot'],'Per-principal test requires spare global capacity.')
        status,r=request(port,tokens['alpha'],message('create',body=dict(identity='shared-identity',purpose='text-room')))
        a.require(status==503 and r['body']['code']=='ResourceLimit','Per-principal capacity not enforced.')
        launch('delta','delta');h=await_health(lambda h:h['concurrent']['ready']==4 and h['connected_sessions']==4)
        a.require(all(children[r]['process'].poll() is None for r in ROLES),'Clients did not overlap.')
        record['overlap_pids']=[children[r]['process'].pid for r in ROLES];a.require(len(set(record['overlap_pids']))==4,'Distinct processes required.')
        identities={row['principal']:row['session_id'] for row in h['concurrent']['sessions']}
        status,r=request(port,tokens['spare'],message('create',body=dict(identity='shared-identity',purpose='text-room')))
        a.require(status==503 and r['body']['code']=='ResourceLimit','Global capacity not enforced.')
        for kind in ('poll','close'):
            status,r=request(port,tokens['alpha'],message(kind,session=identities['beta'],body={'after_event':0} if kind=='poll' else {}))
            a.require(status==404 and r['body']['code']=='SessionNotFound','Another principal accessed a session.')
        record['http_isolation_checks']=4
        control('phase1');await_health(lambda h:h['concurrent']['phase']==1 and h['concurrent']['sealed']==4,120)
        delta=children['delta'];progress=read('delta',True)
        a.require(validate_client(progress,delta['run_id'],delta['process'].pid,artifact['manifest_sha256'],profiles[1]['with_janus'],'delta',progress=True)==identities['delta'],'Delta identity mismatch.')
        record['clients']['delta']=dict(pid=delta['process'].pid,progress=progress,intentionally_killed=True)
        control('close-beta');a.require(finish_client('beta')==identities['beta'],'Beta identity mismatch.')
        await_health(lambda h:h['concurrent']['native_closed']==1 and h['live_peers']==3)
        delta['process'].kill();record['clients']['delta']['exit_code']=delta['process'].wait(timeout=15)
        await_health(lambda h:h['expired']==1 and h['concurrent']['native_closed']==2 and h['live_peers']==2,45)
        launch('beta-rejoin','beta',True);h=await_health(lambda h:h['concurrent']['ready']==3 and h['connected_sessions']==3)
        replacement=[row['session_id'] for row in h['concurrent']['sessions'] if row['principal']=='beta' and not row['native_closed']]
        a.require(len(replacement)==1 and replacement[0]!=identities['beta'],'Reconnect reused session state.')
        identities['beta-rejoin']=replacement[0]
        control('phase2');await_health(lambda h:h['concurrent']['phase']==2 and h['concurrent']['sealed']==3,120);control('finish')
        for case in ('alpha','gamma','beta-rejoin'):a.require(finish_client(case)==identities[case],'Client identity mismatch.')
        await_health(lambda h:h['live_peers']==0 and h['concurrent']['native_closed']==5)
        status,_=request(port,admin,{},path='/simurghrtc/probe/stop');a.require(status==204,'Server stop failed.')
        child=children['server']['process'];record['server_exit_code']=child.wait(timeout=30);r=read('server');record['server']=r
        validate_server(r,children['server']['run_id'],child.pid,artifact['manifest_sha256'],application_messages=True)
        validate_concurrent_server(r,identities);a.require(record['server_exit_code']==0,'Server exit failed.')
        with socket.socket() as sock:a.require(sock.connect_ex(('127.0.0.1',port))!=0,'Listener remains open.')
        record['identities']=identities;record['passed']=True
    except (ValueError,OSError,KeyError,subprocess.SubprocessError,http.client.HTTPException) as error:record['error']=str(error)
    finally:
        for c in children.values():
            child=c['process']
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=10)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=10)
        for path in secrets_paths:path.unlink(missing_ok=True)
        record['secrets_removed']=all(not p.exists() for p in secrets_paths)
        record['all_processes_stopped']=all(c['process'].poll() is not None for c in children.values())
        record['elapsed_seconds']=round(time.monotonic()-start,3)
        (out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(dict(passed=record['passed'],error=record.get('error'),evidence=str(out/'result.json'))))
    return 0 if record['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
