#!/usr/bin/env python3
"""Measure generated Opus audio between packaged Unreal Game and dedicated Server processes."""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import secrets
import socket
import subprocess
import time
import uuid
import mobile_artifacts as a
from run_signaling_probe import server_bundle_id
from run_mobile_probe import read_report

MARKER = 'SIMURGHRTC_SHARED_AUDIO_REPORT '


def validate(report, run_id, pid, manifest, server, with_janus):
    a.require(type(report) is dict,'Missing shared audio report.')
    expected = dict(schema=1,run_id=run_id,pid=pid,server=server,server_build=server,editor_build=False,
                    with_janus=with_janus,loopback_only=True,audio_hardware_opened=False,passed=True,error='',
                    phases=4,checks=0 if server else 4,pending_cleanup=0,
                    client_native_closed=0 if server else 1,client_remote_closed=0 if server else 1,
                    artifact_manifest_sha256=manifest)
    if server: expected.update(server_live_peers=0,server_closed=1)
    for key,value in expected.items():
        a.require(type(report.get(key)) is type(value) and report[key]==value,'Unexpected shared audio report: '+key)
    a.require(report.get('engine_version','').startswith('5.8.3-'),'Expected Unreal 5.8.3.')
    a.require(type(report.get('quiet_ticks')) is int and report['quiet_ticks']>=8,'Missing quiet cleanup interval.')
    rows=report.get('measurements')
    a.require(type(rows) is list and len(rows)==4,'Missing audio measurement phases.')
    for phase,row in enumerate(rows):
        a.require(type(row.get('phase')) is int and row['phase']==phase,'Unexpected measurement order.')
        a.require(type(row.get('samples')) is int and row['samples']>=48000,'Insufficient decoded samples.')
        for key in ('correlation','rms'):
            a.require(type(row.get(key)) in (int,float) and math.isfinite(row[key]) and row[key]>=0,'Invalid tone measurement: '+key)
        a.require(row['correlation']<1.0001 and row['rms']<=1,'Out-of-range tone measurement.')
        if server and phase==1:a.require(row['rms']<.002,'Muted source was audible.')
        else:a.require(row['correlation']>.9 and row['rms']>.1,'Decoded tone is missing or incorrect.')


def run(sp,cp,server,client,out):
    a.require(not out.exists(),'Use a fresh evidence directory.')
    artifact=a.validate(sp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(artifact==a.validate(cp/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64'),'Native artifacts differ.')
    profiles=[a.read_json(p/'Plugins/SimurghRTC/SimurghRTC.profile.json') for p in (sp,cp)]
    a.require(profiles[0]['profile']=='server' and profiles[1]['with_direct'] and profiles[1]['with_media'],'Expected Server and media-enabled Direct client profiles.')
    for exe in (server,client):
        subprocess.run(['codesign','--verify','--deep','--strict',str(exe.parents[2])],check=True,capture_output=True)
        a.require(subprocess.check_output(['xcrun','lipo','-archs',str(exe)],text=True).strip()=='arm64','Expected ARM64 packages.')
    out.mkdir(parents=True);token=secrets.token_urlsafe(32);children=[];secret_files=[];run_id=str(uuid.uuid4())
    result=dict(schema=1,passed=False,run_id=run_id,artifact=artifact,profile=profiles[1]['profile'],reports={},
                server_executable_sha256=a.digest(server),client_executable_sha256=a.digest(client),
                scope='Mac ARM64 packaged shared C++ API to dedicated Server; Direct HTTP signaling, generated Opus, local ICE; no hardware audio or Blueprint graph execution')
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    try:
        for index,exe in enumerate((server,client)):
            role='server' if index==0 else 'client';rid=str(uuid.uuid4())
            directory=Path.home()/'Library/Containers'/server_bundle_id(exe)/'Data/Library/Caches/SimurghRTC/SharedAudio'/run_id/role
            directory.mkdir(parents=True,exist_ok=False);secret=directory/'token'
            with secret.open('x') as stream:secret.chmod(0o600);stream.write(token)
            secret_files.append(secret)
            command=[str(exe),'/Engine/Maps/Entry','-nullrhi','-nosound','-unattended','-stdout','-FullStdOutLogOutput',
                     '-UserDir='+str(directory),'-SimurghRTCSharedAudioProbe','-SimurghRTCSessionLoopbackProbe',
                     '-SimurghRTCSharedAudioRunId='+rid,'-SimurghRTCSharedAudioTokenFile='+str(secret)]
            command+=['-SimurghRTCSharedAudioServer','-SimurghRTCSharedAudioPort='+str(port)] if index==0 else ['-SimurghRTCSharedAudioEndpoint=http://127.0.0.1:'+str(port)+'/simurghrtc/v1/signal']
            (out/(role+'-command.json')).write_text(json.dumps(command,indent=2)+'\n');log=out/(role+'.log')
            with log.open('w') as stream:process=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
            children.append(dict(process=process,role=role,run_id=rid,log=log))
            if index==0:
                deadline=time.monotonic()+45
                while 'SIMURGHRTC_SHARED_AUDIO_READY '+rid not in log.read_text(errors='replace'):
                    a.require(process.poll() is None,'Server exited before readiness.')
                    a.require(time.monotonic()<deadline,'Server readiness timed out.');time.sleep(.1)
        result['exit_codes']={}
        for c in children:
            result['exit_codes'][c['role']]=c['process'].wait(timeout=110)
            result['reports'][c['role']]=read_report(c['log'].read_text(errors='replace'),c['run_id'],MARKER)
        for index,c in enumerate(children):
            validate(result['reports'][c['role']],c['run_id'],c['process'].pid,artifact['manifest_sha256'],index==0,profiles[index]['with_janus'])
            a.require(result['exit_codes'][c['role']]==0,'Audio process failed: '+c['role'])
        a.require(children[0]['process'].pid!=children[1]['process'].pid,'Expected separate processes.')
        a.require(a.digest(server)==result['server_executable_sha256'] and a.digest(client)==result['client_executable_sha256'],'Package changed during validation.')
        result['passed']=True
    except (OSError,ValueError,subprocess.SubprocessError) as error:result['error']=str(error)
    finally:
        for c in children:
            if c['process'].poll() is None:
                c['process'].terminate()
                try:c['process'].wait(timeout=10)
                except subprocess.TimeoutExpired:c['process'].kill();c['process'].wait(timeout=10)
        for secret in secret_files:secret.unlink(missing_ok=True)
        result['credentials_removed']=all(not path.exists() for path in secret_files)
        result['processes_exited']=all(c['process'].poll() is not None for c in children)
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('server-project','client-project','server-executable','client-executable','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();result=run(*[getattr(args,n).resolve() for n in ('server_project','client_project','server_executable','client_executable','output')])
    print(json.dumps(dict(passed=result['passed'],error=result.get('error'))));raise SystemExit(0 if result['passed'] else 1)
