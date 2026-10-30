#!/usr/bin/env python3
"""Run packaged C++/Blueprint data clients over verified HTTPS and forced TURN routes."""
from contextlib import nullcontext
from udp_impairment import UdpImpairment
from network_validation import NETWORK_SCENARIOS, network_case, validate_remote_network, validate_impairment, impair_connection
import argparse
import http.client
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import uuid
import mobile_artifacts as a
from network_fixtures import HttpsProxy, TurnFixture, certificates
from run_data_example_probe import MAPS, validate_example
from run_direct_probe import request, wait_until, validate_server
from run_mobile_data_example_probe import MobileClient
from run_mobile_probe import read_report
from run_signaling_probe import server_bundle_id
from channel_validation import CHANNEL_SCENARIOS, channel_case, validate_remote_channel


def validate_route(transport, protocol):
    for key in ('local_candidate','remote_candidate'):
        a.require(type(transport.get(key)) is int and transport[key]==4,'Selected '+key+' is not a relay.')
    a.require(type(transport.get('ice_route_samples')) is int and transport['ice_route_samples']>0,'Missing selected route stats.')
    a.require(type(transport.get('relay_protocol')) is int and transport['relay_protocol']==(1 if protocol=='udp' else 2),
              'Selected TURN protocol differs.')


def validate_tls_rejection(evidence):
    # Some HTTP stacks check hostname after the TLS handshake, before sending HTTP.
    attempted = evidence.get('tls_failures', 0) > 0 or bool(evidence.get('tls_versions'))
    a.require(evidence.get('requests') == 0 and attempted,
              'Certificate rejection must follow a TLS attempt and precede signaling.')


def recover_connection(client, cid, dest, scenario, gate, proxy, turn, row):
    """Interrupt only fixture-owned traffic, observe Failed, then permit the sample to reconnect."""
    samples=[];started=time.monotonic()
    def progress(predicate):
        report=client.report(cid,dest,progress=True)
        if report:
            a.require(report.get('run_id')==cid,'Wrong recovery progress run.')
            samples.append(dict(elapsed_seconds=round(time.monotonic()-started,3),**report))
            (dest/'progress.json').write_text(json.dumps(samples,indent=2)+'\n')
            if predicate(report):return report
        final=client.report(cid,dest)
        if final:raise ValueError('Recovery ended before the gate: '+str(final.get('error')))
        return None
    first=wait_until(lambda:progress(lambda p:p['completed_rounds']==1 and p['state']==2),35)
    sid=first['session_id'];a.require(uuid.UUID(sid).int!=0,'Missing recovery session identity.')
    requests_before=proxy.requests
    row['outage']=dict(kind=scenario,first_round=first)
    try:
        if scenario=='signaling-loss':proxy.set_outage(True)
        else:turn.pause()
        failed=wait_until(lambda:progress(lambda p:p['failure_observed'] and p['state']==5),75)
        a.require(failed['session_id']==sid and failed['round']==0 and failed['completed_rounds']==1,'Outage changed session or round.')
        a.require(failed['connection_error'] in ([11] if scenario=='signaling-loss' else [2,11]),'Unexpected connection-loss error.')
        observed=time.monotonic()
        responsive=wait_until(lambda:progress(lambda p:time.monotonic()-observed>=3 and p['heartbeats']>=failed['heartbeats']+10),10)
        a.require(responsive['state']==5 and responsive['session_id']==sid and responsive['max_tick_gap_seconds']<2,
                  'Game thread stalled or recovery occurred before the fixture was restored.')
        if scenario=='signaling-loss':a.require(proxy.dropped_requests>0,'Signaling outage did not intercept a request.')
        else:a.require(proxy.requests>requests_before,'Signaling stopped during the relay-only outage.')
        row['outage'].update(failed=failed,still_responsive=responsive,seconds_to_failure=round(observed-started,3),
                             dropped_requests=proxy.dropped_requests,https_requests_during_outage=proxy.requests-requests_before)
    finally:
        proxy.set_outage(False);turn.resume()
    client.write(gate,'ready')
    row['outage']['restored_before_reconnect']=True


def background_connection(client, cid, dest, scenario, proxy, await_health, row):
    """Switch the running app out and back; never synthesize an Unreal lifecycle event."""
    samples=[];started=time.monotonic();pending=scenario=='background-connect'
    def progress(predicate):
        value=client.report(cid,dest,progress=True)
        if value:
            a.require(value.get('run_id')==cid and value.get('error')=='','Wrong or failed lifecycle progress.')
            samples.append(dict(elapsed_seconds=round(time.monotonic()-started,3),**value))
            (dest/'progress.json').write_text(json.dumps(samples,indent=2)+'\n')
            if predicate(value):return value
        final=client.report(cid,dest)
        if final:raise ValueError('Lifecycle probe ended before app resume: '+str(final.get('error')))
        return None
    first=wait_until(lambda:progress(lambda p:p['state']==(1 if pending else 2)
        and p['transports_created']==1 and (proxy.held_requests>0 if pending else p['completed_rounds']==1)),30)
    client.background()
    closed=wait_until(lambda:progress(lambda p:p['background_events']==1 and p['background_shutdown']),45)
    a.require(closed['session_id']==first['session_id'] and closed['state']==4 and closed['in_background']
              and closed['inactive_events']==1 and closed['active_events']==0
              and closed['foreground_events']==0 and closed['late_callbacks']==0,'Background did not retire the original session.')
    a.require(closed['background_pending'] is pending and closed['background_cancelled'] is pending,'Pending shutdown differs.')
    observed=time.monotonic()
    health=await_health(lambda h:h['live_peers']==0,20)
    a.require(health['created']==health['closed']==(0 if pending else 1),'Old Server session did not close while the app was away.')
    # Keep the OS transition long enough to cover suspension and Server lease expiry.
    quiet=wait_until(lambda:progress(lambda p:time.monotonic()-observed>=8),12)
    a.require(quiet['in_background'] and quiet['foreground_events']==0 and quiet['completions']==closed['completions']
              and quiet['late_callbacks']==0,'Callbacks or a reconnect occurred while backgrounded.')
    client.assert_same_process();proxy.set_hold(False);client.foreground()
    row['application_lifecycle']=dict(first=first,background=closed,quiet=quiet,server_while_backgrounded=health,
        same_process_resumed=True,held_requests=proxy.held_requests,background_observed_seconds=round(time.monotonic()-observed,3))


def run(args):
    recovery=getattr(args,'recovery',False)
    background=getattr(args,'background',False)
    channels=getattr(args,'channels',False)
    impairment=getattr(args,'impairment',False)
    output=args.output.resolve();a.require(not output.exists(),'Use a fresh evidence directory.');output.mkdir(parents=True)
    server=args.server_executable.resolve();project=args.server_project.resolve()
    artifact=a.validate(project/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(a.read_json(project/'Plugins/SimurghRTC/SimurghRTC.profile.json')['profile']=='server','Expected dedicated Server profile.')
    subprocess.run(['codesign','--verify','--deep','--strict',str(server.parents[2])],check=True,capture_output=True)
    a.require(subprocess.check_output(['xcrun','lipo','-archs',server],text=True).strip()=='arm64','Expected ARM64 Server.')
    certs=output/'certificates';cert_hashes=certificates(certs)
    result=dict(schema=1,passed=False,cases={},server_artifact=artifact,server_executable_sha256=a.digest(server),
                certificate_sha256=cert_hashes,signaling='verified HTTPS to local TLS proxy; Unreal HTTPServer behind it',
                turn_fixture=a.read_json(args.turn_fixture),physical_ios_tested=False,recovery_probes=recovery,background_probes=background,channel_probes=channels,impairment_probes=impairment)
    simulator=None;client=None;started=time.monotonic()
    def sim(*command,check=True,timeout=120):
        r=subprocess.run(['xcrun','simctl',*map(str,command)],capture_output=True,text=True,timeout=timeout)
        with (output/'simulator-commands.log').open('a') as f:f.write(json.dumps(list(map(str,command)))+'\n'+r.stdout+r.stderr+'\n')
        a.require(not check or r.returncode==0,'Simulator command failed.');return r.stdout.strip()
    try:
        device=args.device
        if args.platform=='IOSSimulator':
            # Trust is added only to a new fixture-owned simulator, deleted in finally.
            devices=json.loads(sim('list','devices','available','--json'))['devices']
            matches=[(runtime,d) for runtime,items in devices.items() for d in items if d['udid']==device]
            a.require(len(matches)==1,'Select an available source simulator.')
            runtime,source=matches[0]
            simulator=sim('create','SimurghRTC-Network-'+str(uuid.uuid4()),source['deviceTypeIdentifier'],runtime)
            a.require(str(uuid.UUID(simulator)).upper()==simulator.upper(),'Unexpected simulator identity.')
            device=simulator;sim('boot',device);sim('bootstatus',device,'-b',timeout=180)
            sim('keychain',device,'add-root-cert',certs/'ca.pem')
            result['disposable_simulator']=device
        client=MobileClient(args.client_project.resolve(),args.app.resolve(),args.platform,device,output,args.adb,args.aapt)
        client.install();result['client']=client.identity
        with TurnFixture(args.turn_fixture.resolve(),output/'turn',args.listen_ip) as turn:
            cases=[(implementation,'messages',route,'trusted') for route in ('direct','udp','tcp') for implementation in MAPS]
            cases += [('cpp','unauthorized','direct','trusted'),('cpp','tls-rejected','direct','untrusted'),('blueprint','tls-rejected','direct','wrong-host')]
            if recovery:
                cases=[(implementation,scenario,route,'trusted') for implementation in MAPS
                       for scenario,route in [('reconnect','direct'),('reconnect','udp'),('signaling-loss','direct'),('relay-loss','udp')]]
            if background:
                cases=[(implementation,scenario,route,'trusted') for implementation in MAPS
                       for scenario,route in [('background-connected','direct'),('background-connected','udp'),('background-connect','direct')]]
            if channels:
                cases=[(implementation,scenario,'direct','trusted') for implementation in MAPS for scenario in CHANNEL_SCENARIOS]
                cases += [(implementation,'channels-0','udp','trusted') for implementation in MAPS]
            if impairment:cases=[(implementation,scenario,'udp','trusted') for implementation in MAPS for scenario in NETWORK_SCENARIOS]
            for implementation,scenario,route,certificate in cases:
                name='-'.join([implementation,scenario,route,certificate]);dest=output/name;dest.mkdir()
                run_id=str(uuid.uuid4());cid=str(uuid.uuid4());token=secrets.token_urlsafe(32)
                folder=Path.home()/'Library/Containers'/server_bundle_id(server)/'Data/Library/Caches/SimurghRTC/Network'/run_id
                folder.mkdir(parents=True,exist_ok=False);credential=folder/'token';ice_path=folder/'ice.json';process=None;udp=None;proxy=None
                for path,value in [(credential,token),*([(ice_path,json.dumps(turn.config_for(transport=route)))] if route!='direct' else [])]:
                    with path.open('x') as f:os.chmod(path,0o600);f.write(value)
                loopback=client.simulator and route=='direct'
                with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
                row=dict(implementation=implementation,scenario=scenario,route=route,certificate=certificate,passed=False)
                result['cases'][name]=row
                def health():
                    status,value=request(port,None,path='/simurghrtc/probe/direct',method='GET')
                    a.require(status==200 and value['run_id']==run_id and value['pid']==process.pid,'Wrong Server process.')
                    return value
                def await_health(predicate,timeout=30):
                    def poll():
                        try:
                            value=health();return value if predicate(value) else None
                        except (OSError,http.client.HTTPException):return None
                    return wait_until(poll,timeout)
                try:
                    command=[str(server),'-SimurghRTCDirectServerProbe','-SimurghRTCDirectScenario=example',
                        '-SimurghRTCDirectRunId='+run_id,'-SimurghRTCDirectPort='+str(port),
                        '-SimurghRTCDirectTokenFile='+str(credential),'-UserDir='+str(folder),
                        '-nullrhi','-nosound','-unattended','-stdout','-FullStdOutLogOutput']
                    if not loopback:command+=['-SimurghRTCDirectNetworkProbe']
                    if route!='direct':command+=['-SimurghRTCIceConfigFile='+str(ice_path)]
                    (dest/'server-command.json').write_text(json.dumps(command,indent=2)+'\n')
                    with (dest/'server.log').open('w') as stream:process=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
                    await_health(lambda h:h['running'])
                    with HttpsProxy(port,certs/(certificate+'.pem'),certs/(certificate+'.key'),
                                    message_limit=32768 if scenario=='channels-small' else None,close_race=impairment) as proxy, \
                            (UdpImpairment(args.listen_ip,turn.port) if impairment else nullcontext()) as udp:
                        if scenario=='background-connect':proxy.set_hold(True)
                        ice=turn.config_for(transport=route) if route!='direct' else None
                        if impairment:ice['servers'][0]['urls']=[f'turn:{args.listen_ip}:{udp.port}?transport=udp']
                        gate=client.launch(cid,implementation,scenario,proxy.port,token+('-wrong' if scenario=='unauthorized' else ''),
                            https=True,ice=ice,
                            ca=None if client.simulator else certs/'ca.pem')
                        pid=client.pid
                        if scenario in ('signaling-loss','relay-loss'):recover_connection(client,cid,dest,scenario,gate,proxy,turn,row)
                        if background:background_connection(client,cid,dest,scenario,proxy,await_health,row)
                        if impairment:impair_connection(client,cid,dest,scenario,gate,udp,row)
                        report=wait_until(lambda:client.report(cid,dest),90)
                        row['client']=report;(dest/'probe.json').write_text(json.dumps(report,indent=2)+'\n')
                        validate_example(report,cid,pid,client.artifact['manifest_sha256'],client.config['with_janus'],implementation,scenario,
                            platform='IOS' if client.simulator else 'Android',simulator=client.simulator,loopback=loopback)
                        a.require(report.get('https') is True and report.get('relay_only') is (route!='direct'),'Client network configuration differs.')
                        row['tls']=proxy.report()
                        if impairment:a.require(proxy.close_races==proxy.closed_close_responses==1,'Missing authenticated already-closed acknowledgement.')
                        if scenario=='channels-small':
                            a.require(proxy.sdp_requests>=1 and proxy.sdp_responses>=1,'Lower SDP limit was not negotiated in both directions.')
                        if scenario=='tls-rejected':
                            validate_tls_rejection(row['tls'])
                        else:a.require(proxy.requests>0 and proxy.tls_versions,'Missing HTTPS traffic.')
                        client.stop()
                    if impairment:
                        row['impairment']=udp.report();validate_impairment(row['impairment'],scenario)
                    current=await_health(lambda h:h['live_peers']==0,20);row['health']=current
                    request(port,token,{},path='/simurghrtc/probe/stop');code=process.wait(timeout=30)
                    report=read_report((dest/'server.log').read_text(),run_id,'SIMURGHRTC_DIRECT_REPORT ');row['server']=report
                    row['server_exit_code']=code
                    validate_server(report,run_id,process.pid,artifact['manifest_sha256'],application_messages=True,loopback=loopback)
                    a.require(code==0 and report['records']==report['echo_pending']==0,'Server cleanup incomplete.')
                    expected=2 if recovery or scenario=='background-connected' else int(channels or impairment or scenario in ('messages','background-connect'))
                    messages=network_case(scenario)['received'] if impairment else channel_case(scenario)['messages'] if channels else 2*expected
                    a.require(report['created']==report['connected']==report['closed']==expected and report['data_received']==report['data_sent']==messages,'Server totals differ.')
                    if channels:validate_remote_channel(report,scenario)
                    if impairment:validate_remote_network(report,scenario)
                    if route!='direct':validate_route(report['last_transport'],route)
                    with socket.socket() as sock:a.require(sock.connect_ex(('127.0.0.1',port))!=0,'Server listener remains open.')
                    row['passed']=True;print('PASS '+name,flush=True)
                finally:
                    if udp:row['impairment']=udp.report()
                    if proxy:row['tls']=proxy.report()
                    if process and process.poll() is None and not row['passed']:
                        try:
                            row['failure_health']=health()
                            request(port,token,{},path='/simurghrtc/probe/stop');process.wait(timeout=10)
                        except (OSError,ValueError,subprocess.SubprocessError,http.client.HTTPException):pass
                    if process and process.poll() is None:
                        process.terminate()
                        try:process.wait(timeout=10)
                        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
                    credential.unlink(missing_ok=True);ice_path.unlink(missing_ok=True)
                    row.update(server_stopped=process is None or process.poll() is not None,server_secrets_removed=not credential.exists() and not ice_path.exists())
                    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        result['passed']=True
    except (ValueError,OSError,KeyError,subprocess.SubprocessError,http.client.HTTPException) as error:result['error']=str(error)
    finally:
        try:
            if client:client.cleanup()
            result.update(client_stopped=True,client_secrets_removed=True,reverse_ports_removed=True)
        except (ValueError,OSError,subprocess.SubprocessError) as error:result.update(passed=False,cleanup_error=str(error))
        if simulator:
            try:
                sim('shutdown',simulator,check=False);sim('delete',simulator);result['disposable_simulator_deleted']=True
            except (ValueError,OSError,subprocess.SubprocessError) as error:result.update(passed=False,simulator_cleanup_error=str(error))
        for path in certs.iterdir():
            if path.suffix in ('.key','.csr','.srl','.ext'):path.unlink()
        result['certificate_keys_removed']=not list(certs.glob('*.key'))
        result['elapsed_seconds']=round(time.monotonic()-started,3)
        (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main(*, recovery=False, background=False, channels=False, impairment=False):
    parser=argparse.ArgumentParser(description='Validate packaged mobile packet loss, reordering, partial-reliability expiry and aggregate backpressure.' if impairment else
                                  'Validate packaged mobile DataChannel options, payload limits, ordering and backpressure.' if channels else
                                  'Validate packaged mobile app background/resume through actual OS lifecycle events.' if background else
                                  'Validate packaged mobile reconnect and controlled HTTPS/TURN loss recovery.' if recovery else __doc__)
    for name in ('server-project','server-executable','client-project','app','output','turn-fixture'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--platform',choices=['Android','IOSSimulator'],required=True);parser.add_argument('--device',required=True)
    parser.add_argument('--listen-ip',default='127.0.0.1',help='Local Mac LAN IPv4 for physical Android; loopback for simulator.')
    parser.add_argument('--adb',type=Path,default=Path.home()/'Library/Android/sdk/platform-tools/adb')
    parser.add_argument('--aapt',type=Path,default=Path.home()/'Library/Android/sdk/build-tools/36.0.0/aapt')
    args=parser.parse_args();args.recovery=recovery;args.background=background;args.channels=channels;args.impairment=impairment
    result=run(args);print(json.dumps(dict(passed=result['passed'],error=result.get('error'))))
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
