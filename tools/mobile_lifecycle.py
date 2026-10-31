"""Real OS lifecycle controls and strict packaged send-deadline acceptance."""
import json
import os
import re
import signal
import time
import uuid
import mobile_artifacts as a
from run_direct_probe import wait_until
from run_mobile_probe import process_alive

LIFECYCLE_SCENARIOS = ('background-connected','background-connect','terminate-connected','terminate-connect','deadline-expire','deadline-release')


def ui_request(client, action):
    """A visible UI driver handles simulator lock/home; no synthesized app callbacks."""
    nonce=str(uuid.uuid4());path=client.output/'ui-request.json';reply=client.output/'ui-response.json'
    request=dict(nonce=nonce,action=action,device=client.device,device_name=client.identity['device_info']['name'])
    path.write_text(json.dumps(request,indent=2)+'\n');print('UI REQUEST '+json.dumps(request),flush=True)
    def response():
        try:d=json.loads(reply.read_text())
        except (FileNotFoundError,json.JSONDecodeError):return None
        if d.get('nonce')!=nonce:return None
        a.require(d.get('action')==action and d.get('device')==client.device and d.get('verified') is True,'UI action was not verified on this fixture simulator: '+str(d.get('observed','')))
        return d
    result=wait_until(response,180)
    with (client.output/'ui-actions.jsonl').open('a') as f:f.write(json.dumps(dict(request=request,response=result))+'\n')
    path.unlink();return result


def android_lock_state(client):
    power=client.command(client.adb+['shell','dumpsys','power']).stdout
    policy=client.command(client.adb+['shell','dumpsys','window','policy']).stdout
    state=re.search(r'^\s*mWakefulness=(\w+)',power,re.M)
    keyguard=re.search(r'KeyguardServiceDelegate.*?\bshowing=(true|false)',policy,re.S)
    a.require(state is not None and keyguard is not None,'Missing Android OS lock-state evidence.')
    return dict(wakefulness=state[1],keyguard_showing=keyguard[1]=='true')


def lock_client(client):
    client.assert_same_process()
    if client.simulator:
        result=ui_request(client,'lock');client.screen_lock_requested=True
        return result
    before=android_lock_state(client)
    a.require(before==dict(wakefulness='Awake',keyguard_showing=False),'Start the lock suite with the Galaxy awake and unlocked.')
    client.screen_lock_requested=True
    client.command(client.adb+['shell','input','keyevent','KEYCODE_SLEEP'])
    def locked():
        state=android_lock_state(client)
        return state if state['wakefulness'] in ('Asleep','Dozing') and state['keyguard_showing'] else None
    return dict(before=before,locked=wait_until(locked,20),action='KEYCODE_SLEEP')


def unlock_client(client):
    if client.simulator:
        result=ui_request(client,'unlock');client.screen_lock_requested=False;return result
    client.command(client.adb+['shell','input','keyevent','KEYCODE_WAKEUP'])
    client.command(client.adb+['shell','wm','dismiss-keyguard'],check=False)
    def unlocked():
        state=android_lock_state(client)
        return state if state==dict(wakefulness='Awake',keyguard_showing=False) else None
    # A secure device may require its owner to unlock. Never change its lock settings.
    print('ANDROID UNLOCK: unlock the Galaxy if its secure lock screen remains visible.',flush=True)
    state=wait_until(unlocked,90);client.screen_lock_requested=False
    return dict(unlocked=state,action='KEYCODE_WAKEUP; dismiss nonsecure keyguard or owner unlock')



def resume_after_unlock(client,cid,dest):
    """Let the OS finish its asynchronous activation before explicitly foregrounding."""
    until=time.monotonic()+5
    while time.monotonic()<until:
        progress=client.report(cid,dest,progress=True)
        if (progress and not progress.get('in_background',True)) or client.report(cid,dest):
            return
        time.sleep(.1)
    client.foreground()


def terminate_client(client):
    client.assert_same_process();pid=client.pid
    if client.simulator:
        os.kill(pid,signal.SIGKILL)
        wait_until(lambda:not process_alive(pid),10)
    else:
        client.command(client.adb+['shell','run-as',client.app_id,'kill','-9',str(pid)])
        def gone():
            current=client.command(client.adb+['shell','pidof',client.app_id],check=False).stdout.split()
            return str(pid) not in current and not current
        wait_until(gone,10)
    return dict(pid=pid,signal='SIGKILL',process_absent=True,graceful_client_cleanup_claimed=False)


def live_progress(client,cid,dest,predicate):
    p=client.report(cid,dest,progress=True)
    if p:
        (dest/'progress-latest.json').write_text(json.dumps(p,indent=2)+'\n')
        a.require(p.get('run_id')==cid and not p.get('error'),'Wrong or failed live probe.')
        if predicate(p):return p
    report=client.report(cid,dest)
    if report is not None:
        (dest/'early-final.json').write_text(json.dumps(report,indent=2)+'\n')
        raise ValueError('Probe finished before the host action: '+str(report.get('error','see early-final.json')))
    return None


def deadline_connection(client,cid,dest,scenario,gate,await_health,row):
    progress=lambda predicate:live_progress(client,cid,dest,predicate)
    first=wait_until(lambda:progress(lambda p:p['send_deadline']['phase']==1 and p['send_deadline']['peak_buffered_bytes']>0),30)
    observed=time.monotonic()
    waiting=wait_until(lambda:progress(lambda p:time.monotonic()-observed>=3 and p['heartbeats']>=first['heartbeats']+10),8)
    a.require(waiting['state']==2 and waiting['connection_error']==0 and waiting['received']==0,'Queue failed or delivered while its native send gate was held.')
    health=await_health(lambda h:h['connected']==1 and h['live_peers']==1)
    a.require(health['data_received']==0,'A gated payload reached the Server.')
    row['send_gate']=dict(first=first,waiting=waiting,server_while_blocked=health)
    if scenario=='deadline-expire':
        failed=wait_until(lambda:progress(lambda p:p['send_deadline']['phase']==2 and p['state']==5),40)
        a.require(failed['connection_error']==8,'Queued send deadline did not preserve Timeout.')
        row['send_gate']['failure']=failed
    release=gate.with_name('send-release');client.write(release,'release\n')
    row['send_gate']['released_after_failure']=scenario=='deadline-expire'


def validate_deadline(report, scenario):
    a.require(scenario in ('deadline-expire','deadline-release'),'Unknown deadline case.')
    expire=scenario=='deadline-expire';d=report.get('send_deadline',{})
    for key,value in dict(expire=expire,phase=3,admitted=2,received=0 if expire else 2).items():
        a.require(type(d.get(key)) is type(value) and d[key]==value,'Invalid send deadline evidence: '+key)
    elapsed=d.get('settled_after_seconds');quiet=d.get('quiet_seconds');peak=d.get('peak_buffered_bytes')
    a.require(type(elapsed) in (int,float) and (30<=elapsed<40 if expire else 3<=elapsed<30),'Wrong queued send deadline interval.')
    a.require(type(quiet) in (int,float) and 3<=quiet<15,'Missing post-deadline quiet interval.')
    a.require(type(peak) is int and 0<peak<=262144,'Missing bounded queued payload.')
    a.require(report.get('connection_errors')==([8] if expire else []) and report.get('rounds_completed')==1,'Unexpected queued send failure/rounds.')
    a.require(0<=report.get('max_tick_gap_seconds',99)<2 and report.get('heartbeats',0)>=30,'Deadline blocked the game thread.')


def validate_termination(row):
    t=row['termination'];before=t['before'];after=row['client'];pending=row['scenario']=='terminate-connect'
    a.require(t['signal']=='SIGKILL' and t['process_absent'] is True and t['graceful_client_cleanup_claimed'] is False,'Missing actual process death.')
    a.require(before['state']==(1 if pending else 2) and before['transports_created']==1,'Wrong termination phase.')
    a.require(before['sent']==before['received']==(0 if pending else 2) and before['exact_payloads'] is (not pending),'Wrong pre-termination payloads.')
    a.require(t['server_after_death']['created']==t['server_after_death']['closed']==t['server_after_death']['expired']==1
              and t['server_after_death']['live_peers']==0,'Orphan Server session was not reclaimed by its lease.')
    a.require(after['pid']!=t['pid'] and after['session_id']!=before['session_id'] and t['final_report_absent'] is True,'Termination did not require a fresh process/session.')
