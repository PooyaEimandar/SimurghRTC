"""Acceptance counters for the packaged shared-API DataChannel cases."""
import mobile_artifacts as a
import uuid

CHANNEL_SCENARIOS = tuple('channels-'+str(i) for i in range(6)) + ('channels-small',)


def channel_case(scenario):
    a.require(scenario in CHANNEL_SCENARIOS, 'Unknown channel-validation case.')
    mode = 0 if scenario == 'channels-small' else int(scenario[-1])
    limit = 32768 if scenario == 'channels-small' else 65536
    pressure = mode < 2
    return dict(mode=mode, limit=limit, ordered=mode % 2 == 0, reliability=mode//2,
                value=0 if mode < 2 else (0 if mode == 2 else 2) if mode < 4 else 10000,
                pressure=pressure, messages=38+(262144//limit+66 if pressure else 0))


def validate_channels(report, scenario):
    c=channel_case(scenario);data=report.get('channel_validation',{})
    expected=dict(mode=c['mode'],phase=9,maximum_message_bytes=c['limit'],messages=c['messages'],
                  text_messages=4,binary_messages=c['messages']-4,rejected_sends=5 if c['pressure'] else 3,
                  burst_messages=32,byte_queue_rejections=int(c['pressure']),slot_queue_rejections=int(c['pressure']),
                  byte_queue_recovered=c['pressure'],slot_queue_recovered=c['pressure'],options_checked=True,all_echoes_received=True)
    for key,value in expected.items():
        a.require(type(data.get(key)) is type(value) and data[key]==value,'Invalid packaged channel evidence: '+key)
    peak=data.get('peak_buffered_bytes');events=data.get('buffer_events')
    a.require(type(peak) is int and 0<=peak<=262144 and (not c['pressure'] or peak==262144),'Missing bounded queue saturation.')
    a.require(type(events) is int and events>=2,'Missing channel buffer events.')
    a.require(type(report.get('rounds_completed')) is int and report['rounds_completed']==1
              and report.get('connection_errors')==[],'Channel run did not complete cleanly.')
    gap=report.get('max_tick_gap_seconds');ticks=report.get('heartbeats')
    a.require(type(gap) in (int,float) and 0<=gap<2 and type(ticks) is int and ticks>=10,'Channel load blocked the game thread.')


def validate_remote_channel(server, scenario):
    c=channel_case(scenario)
    # The public client facade and private Server transport have different IDs.
    ids=server.get('connected_session_ids')
    a.require(type(ids) is list and len(ids)==1 and type(ids[0]) is str and str(uuid.UUID(ids[0]))==ids[0]
              and uuid.UUID(ids[0]).int!=0 and server.get('closed_session_ids')==ids,'Missing correlated Server session cleanup.')
    expected=dict(session_id=ids[0],label='sr107-channels-'+str(c['mode']),ordered=c['ordered'],
                  reliability=c['reliability'],reliability_value=c['value'],maximum_message_bytes=c['limit'],remote=True)
    rows=server.get('channel_options')
    a.require(type(rows) is list and len(rows)==1,'Missing unique remote channel evidence.')
    for key,value in expected.items():
        a.require(type(rows[0].get(key)) is type(value) and rows[0][key]==value,'Remote channel differs: '+key)
    a.require(type(server.get('data_channels_opened')) is int and server['data_channels_opened']==1
              and type(server.get('data_channels_closed')) is int and server['data_channels_closed'] in (0,1),'Remote channel lifecycle differs.')
