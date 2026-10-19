#!/usr/bin/env python3
"""Run the C++ and actual Blueprint lifecycle examples in UE 5.8.3 and retain evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time
import uuid
from run_native_probe import read_runtime_log
from prepare_profile import documents

CLIENT_PROFILES = ('full', 'direct-only', 'janus-only', 'data-only')


def profile_features(profile: str, provider: str | None = None) -> tuple[dict, str]:
    if profile not in CLIENT_PROFILES:
        raise ValueError('Lifecycle examples require a client profile.')
    _, manifest = documents(profile)
    features = {key: manifest[key] for key in ('with_direct', 'with_janus', 'with_media', 'with_janus_extensions')}
    provider = provider or ('DirectUnreal' if features['with_direct'] else 'Janus')
    flag = {'DirectUnreal': 'with_direct', 'Janus': 'with_janus'}.get(provider)
    if flag is None or not features[flag]:
        raise ValueError(f'{provider} is excluded from the {profile} profile.')
    return features, provider


def read_report(output: Path, log: str) -> tuple[dict, str]:
    path = output / 'lifecycle.json'
    if path.is_file():
        return json.loads(path.read_text()), 'file'
    marker = 'SIMURGHRTC_LIFECYCLE_REPORT '
    reports = [json.loads(line.split(marker, 1)[1]) for line in log.splitlines() if marker in line]
    if len(reports) != 1:
        raise ValueError('Expected exactly one lifecycle report in the runtime log.')
    return reports[0], 'runtime_log'


def validate_report(report: dict, run_id: str, pid: int, editor: bool, profile: str, provider: str | None = None) -> None:
    features, provider = profile_features(profile, provider)
    expected = {'schema': 2, 'api_contract_version': 1, 'passed': True, 'run_id': run_id, 'pid': pid,
                'editor_build': editor, **features, 'profile_checked': True,
                'janus_blueprint': 'passed' if features['with_janus_extensions'] else 'excluded',
                'network_transport_tested': False, 'provider': provider, 'error': ''}
    for key, value in expected.items():
        if type(report.get(key)) is not type(value) or report.get(key) != value:
            raise ValueError(f'Unexpected {key}: {report.get(key)!r}; expected {value!r}')
    if not report.get('engine_version', '').startswith('5.8.3-'):
        raise ValueError('UE 5.8.3 is required.')
    for implementation in ('cpp', 'blueprint'):
        if report.get(implementation) != {'passed': True, 'end_play_checked': True,
                                         'outcomes': ['Disconnect', 'Connect', 'Cancel'], 'errors': []}:
            raise ValueError(f'{implementation} did not pass all lifecycle branches and EndPlay.')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['editor-game', 'game'])
    parser.add_argument('--project', type=Path)
    parser.add_argument('--profile', required=True, choices=CLIENT_PROFILES)
    parser.add_argument('--provider', choices=['DirectUnreal', 'Janus'])
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    try:
        _, args.provider = profile_features(args.profile, args.provider)
    except ValueError as error:
        parser.error(str(error))
    executable, output = args.executable.resolve(), args.output.resolve()
    if not executable.is_file():
        parser.error('Executable must exist.')
    if args.kind == 'editor-game' and (not args.project or not args.project.is_file()):
        parser.error('editor-game requires --project.')
    if args.kind == 'game' and args.project:
        parser.error('Packaged games use their staged project; omit --project.')
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('Timeout must be finite and positive.')
    if output.exists() and any(output.iterdir()):
        parser.error('Output must be new or empty.')
    output.mkdir(parents=True, exist_ok=True)
    run_id = str(uuid.uuid4())
    command = [str(executable)]
    if args.project:
        command.append(str(args.project.resolve()))
    command += ['/Engine/Maps/Entry', '-game', '-unattended', '-NullRHI', '-nosound', '-nosplash', '-SkipZenStore',
                '-SimurghRTCLifecycleProbe', f'-SimurghRTCLifecycleRunId={run_id}', f'-SimurghRTCLifecycleProvider={args.provider}',
                f'-SimurghRTCLifecycleReport={output / "lifecycle.json"}', f'-abslog={output / "unreal.log"}']
    digest = hashlib.sha256()
    with executable.open('rb') as binary:
        for block in iter(lambda: binary.read(1024 * 1024), b''):
            digest.update(block)
    record = {'schema': 1, 'command': command, 'profile': args.profile, 'kind': args.kind, 'run_id': run_id,
              'executable_sha256': digest.hexdigest(),
              'passed': False}
    started, process = time.monotonic(), None
    try:
        with (output / 'process.log').open('w') as log:
            process = subprocess.Popen(command, cwd=executable.parent, stdout=log, stderr=subprocess.STDOUT)
            record['exit_code'] = process.wait(timeout=args.timeout)
        record['runtime_log'], log = read_runtime_log(output)
        report, record['report_source'] = read_report(output, log)
        record['lifecycle'] = report
        validate_report(report, run_id, process.pid, args.kind == 'editor-game', args.profile, args.provider)
        if record['exit_code'] != 0:
            raise ValueError('The example process did not exit cleanly.')
        if record['report_source'] == 'runtime_log':
            (output / 'lifecycle.json').write_text(json.dumps(report, indent=2) + '\n')
        record['passed'] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        record['process_stopped'] = process is not None and process.poll() is not None
        record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'evidence': str(output / 'result.json'), 'error': record.get('error')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
