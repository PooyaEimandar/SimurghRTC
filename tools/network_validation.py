"""Acceptance rules and gates for packaged SR-107 packet impairment cases."""
import json
import time
import uuid
import mobile_artifacts as a
from run_direct_probe import wait_until

NETWORK_SCENARIOS = tuple('network-'+str(i) for i in range(7))


def network_case(scenario):
    a.require(scenario in NETWORK_SCENARIOS, 'Unknown network-validation case.')
    mode = int(scenario[-1]); expiry = 2 <= mode <= 5; multi = mode == 6
    return dict(mode=mode, expiry=expiry, multi=multi, channels=5 if multi else 1,
                received=22 if multi else 2 if expiry else 33,
                sent=22 if multi else 10 if expiry else 33,
                ordered=mode % 2 == 0, reliability=0 if multi else mode//2,
                value=100 if mode in (4,5) else 0)


def validate_network(report, scenario):
    c=network_case(scenario);data=report.get('network_validation',{})
    expected=dict(mode=c['mode'],phase=9,channels=c['channels'],channels_closed=c['channels'],messages=c['received'],
                  expired_admissions=8 if c['expiry'] else 0,rejected_sends=int(c['multi']),
                  empty_channel_at_rejection=0 if c['multi'] else -1,aggregate_queue_recovered=c['multi'],all_expected_received=True)
    for key,value in expected.items():
        a.require(type(data.get(key)) is type(value) and data[key]==value,'Invalid network evidence: '+key)
    quiet=data.get('quiet_seconds')
    a.require(type(quiet) in (int,float) and (10 if c['expiry'] else 2)<=quiet<60,'Missing post-delivery quiet interval.')
    peak=data.get('peak_buffered_bytes')
    a.require(type(peak) is int and 0<=peak<=1048576 and (not c['multi'] or peak==1048576),'Missing bounded aggregate saturation.')
    a.require(type(data.get('buffer_events')) is int and data['buffer_events']>=c['channels'],'Missing buffer events.')
    ids=report.get('channel_ids')
    a.require(type(ids) is list and len(ids)==c['channels'] and len(set(ids))==len(ids)
              and all(str(uuid.UUID(v))==v and uuid.UUID(v).int for v in ids),'Invalid channel identities.')
    a.require(report.get('rounds_completed')==1 and report.get('connection_errors')==[],'Impairment run did not close cleanly.')
    gap=report.get('max_tick_gap_seconds');ticks=report.get('heartbeats')
    a.require(type(gap) in (int,float) and 0<=gap<2 and type(ticks) is int and ticks>=10,'Impairment blocked the game thread.')


def validate_remote_network(server, scenario):
    c=network_case(scenario);ids=server.get('connected_session_ids');rows=server.get('channel_options')
    a.require(type(ids) is list and len(ids)==1 and str(uuid.UUID(ids[0]))==ids[0]
              and uuid.UUID(ids[0]).int and server.get('closed_session_ids')==ids,'Missing Server session cleanup.')
    a.require(type(rows) is list and len(rows)==c['channels'],'Missing remote channels.')
    for index,row in enumerate(rows):
        expected=dict(session_id=ids[0],label=f"sr107-network-{c['mode']}-{index}",ordered=c['ordered'],
                      reliability=c['reliability'],reliability_value=c['value'],maximum_message_bytes=65536,remote=True)
        for key,value in expected.items():
            a.require(type(row.get(key)) is type(value) and row[key]==value,'Remote channel differs: '+key)
    # Disconnect can retire the Server peer before its last channel event is polled.
    # Require peer cleanup above; the client separately requires every Close completion.
    opened=server.get('data_channels_opened');closed=server.get('data_channels_closed')
    a.require(type(opened) is int and opened==c['channels'] and type(closed) is int and 0<=closed<=c['channels'],
              'Remote channel lifecycle counters differ.')


def validate_impairment(data, scenario):
    c=network_case(scenario)
    a.require(data['closed'] is True and not data['error'] and data['pending_packets']==0,'UDP fixture did not close cleanly.')
    a.require(1<=data['flows']<=8 and data['peak_held_packets']<=16,'Unbounded UDP fixture.')
    directions=data['directions'];out=directions['outbound']
    a.require(out['eligible']>0 and directions['inbound']['eligible']>0,'No encrypted data crossed the fixture.')
    if c['multi']:
        a.require(all(d['impaired']==d['dropped']==d['reordered']==0 for d in directions.values()),'Unexpected pressure-case packet impairment.')
    elif c['expiry']:
        a.require(out['dropped']>0 and out['impaired']==out['dropped'],'Missing complete outbound application-data blackout.')
    else:
        for d in directions.values():a.require(d['dropped']>0 and d['reordered']>0,'Loss and reordering were not exercised in both directions.')


def impair_connection(client,cid,dest,scenario,gate,fixture,row):
    c=network_case(scenario)
    def progress(predicate):
        failure=client.report(cid,dest)
        if failure:(dest/'probe.json').write_text(json.dumps(failure,indent=2)+'\n')
        a.require(failure is None,'Client completed before the impairment gates: '+str(failure.get('error') if failure else ''))
        p=client.report(cid,dest,progress=True)
        if not p:return None
        (dest/'progress-latest.json').write_text(json.dumps(p,indent=2)+'\n')
        a.require(p['run_id']==cid and p['connection_error']==0,'Invalid or failed live client.')
        a.require(not fixture.error,'UDP fixture failed: '+fixture.error)
        return p if predicate(p['network_validation']) else None
    ready=wait_until(lambda:progress(lambda p:p['phase']==1),45)
    fixture.set_mode('pass' if c['multi'] else 'blackout' if c['expiry'] else 'loss-reorder')
    client.write(gate,'start\n')
    release=gate.with_name(gate.name+'.release');client.paths.append(release)
    if c['expiry']:
        sent=wait_until(lambda:progress(lambda p:p['phase']==2 and p['expired_admissions']==8),20)
        dropped_at=wait_until(lambda:time.monotonic() if fixture.report()['directions']['outbound']['dropped']>0 else None,10)
        quiet=wait_until(lambda:progress(lambda p:time.monotonic()-dropped_at>=3),8)
        a.require(quiet['heartbeats']>sent['heartbeats']+10,'Client was stalled during the expiry interval.')
        row['expiry_window']=dict(seconds=round(time.monotonic()-dropped_at,3),sent=sent,quiet=quiet)
    elif not c['multi']:
        row['delivered_under_impairment']=wait_until(lambda:progress(lambda p:p['phase']==3 and p['messages']==c['received']),60)
    fixture.set_mode('pass');client.write(release,'resume\n');row['ready']=ready
