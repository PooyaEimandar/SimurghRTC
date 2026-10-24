#!/usr/bin/env python3
"""Validate local SDP/ICE transport reports; run a packaged Mac dedicated Server probe."""
import argparse
import json
import math
from pathlib import Path
import plistlib
import re
import subprocess
import time
import uuid
import mobile_artifacts as a


def server_bundle_id(executable):
    contents = executable.parent.parent
    a.require(executable.parent.name == 'MacOS' and contents.name == 'Contents'
              and contents.parent.suffix == '.app', 'Use a packaged Mac Server app executable.')
    with (contents / 'Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    identifier = info.get('CFBundleIdentifier')
    a.require(isinstance(identifier, str) and re.fullmatch(r'[A-Za-z][A-Za-z0-9]*(\.[A-Za-z][A-Za-z0-9]*){2,}', identifier)
              and info.get('CFBundleExecutable') == executable.name, 'Invalid Server bundle identity.')
    return identifier


def validate_report(report, run_id, platform, manifest, with_janus, *, simulator=False, server=False, pairs=8):
    a.require(isinstance(report, dict), 'Expected a signaling report object.')
    expected = dict(schema=1, run_id=run_id, platform=platform, editor_build=False, server_build=server,
                    arm64_build=True, simulator_build=simulator, with_janus=with_janus,
                    artifact_manifest_sha256=manifest, native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d',
                    requested_pairs=pairs, completed_pairs=pairs, callbacks_on_game_thread=True,
                    teardown_complete=True, network_transport_tested=True, network_media_tested=False,
                    loopback_only=True, cross_process_tested=False, stun_turn_service_used=False, passed=True, error='')
    def exact(obj, key, value):
        a.require(type(obj.get(key)) is type(value) and obj[key] == value, 'Unexpected signaling ' + key)
    def number(obj, key, minimum=0, maximum=2**53-1, integer=True):
        value = obj.get(key)
        a.require((type(value) is int if integer else type(value) in (int, float))
                  and math.isfinite(value) and minimum <= value <= maximum, 'Invalid signaling ' + key)
        return value
    for key, value in expected.items(): exact(report, key, value)
    a.require(isinstance(report.get('engine_version'), str) and report['engine_version'].startswith('5.8.3-'), 'Expected UE 5.8.3.')
    number(report, 'pid', 1)
    number(report, 'elapsed_seconds', 0, pairs * 30 + 30, False)
    rows = report.get('pairs')
    a.require(isinstance(rows, list) and len(rows) == pairs, 'Incomplete signaling pair inventory.')
    for index, row in enumerate(rows):
        a.require(isinstance(row, dict), 'Invalid signaling pair.')
        for key, value in dict(index=index, mode='media' if index % 2 else 'data',
                               delivery=('early_both', 'early_offer', 'trickle')[index % 3], connected=True, closed=True).items():
            exact(row, key, value)
        accepted = number(row, 'operations_accepted', 10, 256)
        exact(row, 'operations_completed', accepted)
        number(row, 'seconds', 0, 20, False)
        duplicates = number(row, 'duplicates', 2, 64)
        for side in ('offer', 'answer'):
            early = index % 3 == 0 or (index % 3 == 1 and side == 'offer')
            number(row, 'early_' + side + '_ice', 1 if early else 0, 32)
            a.require(type(row.get('early_' + side + '_end')) is bool, 'Invalid early end marker.')
            if early: exact(row, 'early_' + side + '_end', True)
        peers = row.get('peers')
        a.require(isinstance(peers, list) and len(peers) == 2 and all(isinstance(p, dict) for p in peers), 'Expected two peers.')
        local_counts = []
        for peer in peers:
            for key, value in dict(connection=2, dtls=3, sctp_open=True, local_sdp=True, remote_sdp=True,
                                   local_ice_complete=True, remote_ice_complete=True, queued=0).items(): exact(peer, key, value)
            a.require(number(peer, 'ice', 2, 3) in (2, 3), 'ICE is not connected.')
            number(peer, 'protocol', 1, 2)
            number(peer, 'stats_samples', 1)
            number(peer, 'stats_age_ms', 0, 999)
            total = number(peer, 'pairs', 1)
            number(peer, 'succeeded_pairs', 1, total)
            number(peer, 'failed_pairs', 0, total)
            number(peer, 'stun_requests', 1)
            number(peer, 'stun_responses', 1)
            for key in ('send_discards', 'candidate_errors', 'last_candidate_error'): number(peer, key)
            local_counts.append(number(peer, 'local_udp', 0, 32) + number(peer, 'local_tcp', 0, 32))
        a.require(all(local_counts) and duplicates == sum(local_counts), 'Candidate duplicate coverage mismatch.')
        for index, peer in enumerate(peers): exact(peer, 'applied', local_counts[1-index])


def main():
    from run_mobile_probe import read_report
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--project', type=Path, required=True)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--pairs', type=int, default=8)
    args = p.parse_args()
    a.require(2 <= args.pairs <= 32, 'Use 2–32 pairs.')
    output, project, executable = args.output.resolve(), args.project.resolve(), args.executable.resolve()
    a.require(not output.exists() and executable.is_file(), 'Provide an executable and a fresh evidence directory.')
    profile = a.read_json(project / 'Plugins/SimurghRTC/SimurghRTC.profile.json')
    a.require(profile.get('profile') == 'server' and profile.get('with_server') is True and profile.get('with_janus') is False,
              'Use a staged dedicated Server profile.')
    identity = a.validate(project / 'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    app_id = server_bundle_id(executable)
    output.mkdir(parents=True)
    run_id = str(uuid.uuid4())
    record = dict(passed=False, run_id=run_id, artifact=identity, executable_sha256=a.digest(executable), probe_kind='signaling')
    # UE's packaged Mac sample is sandboxed. Keep its report inside its own container.
    user_dir = Path.home() / 'Library/Containers' / app_id / 'Data/Library/Caches/SimurghRTC/SignalingProbes' / run_id
    record['user_directory'] = str(user_dir)
    command = [str(executable), '-nullrhi', '-nosound', '-unattended', '-stdout', '-FullStdOutLogOutput',
               '-SimurghRTCSignalingProbe', '-SimurghRTCSignalingRunId=' + run_id,
               '-SimurghRTCSignalingPairs=' + str(args.pairs), '-SimurghRTCSignalingExit', '-UserDir=' + str(user_dir)]
    record['command'] = command
    process = None
    started = time.monotonic()
    try:
        with (output / 'process.log').open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            record['launch_pid'] = process.pid
            record['exit_code'] = process.wait(timeout=args.pairs * 30 + 60)
        report = read_report((output / 'process.log').read_text(errors='replace'), run_id, 'SIMURGHRTC_SIGNALING_REPORT ')
        if report is not None: record['probe'] = report
        saved = list(user_dir.rglob('signaling-probe-' + run_id + '.json'))
        a.require(len(saved) == 1, 'Expected one saved Server report for this run.')
        saved_report = a.read_json(saved[0])
        a.require(report is None or report == saved_report, 'Saved and logged Server reports differ.')
        report = saved_report
        record['probe'] = report
        validate_report(report, run_id, 'Mac', identity['manifest_sha256'], False, server=True, pairs=args.pairs)
        a.require(report['pid'] == process.pid and process.returncode == 0, 'Server process identity or exit status mismatch.')
        record['passed'] = True
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
    finally:
        if process and process.poll() is None:
            process.terminate()
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=10)
        record['app_stopped'] = process is not None and process.poll() is not None
        record['elapsed_seconds'] = round(time.monotonic()-started, 3)
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(dict(passed=record['passed'], evidence=str(output / 'result.json'), error=record.get('error'))))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
