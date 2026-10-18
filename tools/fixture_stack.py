#!/usr/bin/env python3
"""Build, initialize, run and stop the isolated SR-005 service fixtures."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / 'Fixtures/services'
LOCK = SERVICES / 'sources.lock.json'
IMAGE = 'simurghrtc-fixtures:sr005-v1'
sys.path.insert(0, str(SERVICES))
from provenance import source_hashes


def validate_report(stdout, exit_code, expected_lock, expected_sources):
    marker = 'SIMURGHRTC_SERVICE_REPORT '
    lines = [line[len(marker):] for line in stdout.splitlines() if line.startswith(marker)]
    record = {'passed': False, 'exit_code': exit_code}
    if len(lines) == 1:
        try:
            report = json.loads(lines[0])
            if isinstance(report, dict):
                record.update(report)
        except ValueError:
            pass
    record['passed'] = (exit_code == 0 and len(lines) == 1 and record.get('passed') is True
                        and record.get('lock') == expected_lock and record.get('service_sources') == expected_sources)
    record['exit_code'] = exit_code
    return record


def build_command():
    lock = json.loads(LOCK.read_text())
    args = {'BASE_IMAGE': lock['base_image'], 'DEBIAN_SNAPSHOT': lock['debian_snapshot'],
            'JANUS_COMMIT': lock['janus']['commit'], 'JANUS_SHA256': lock['janus']['archive_sha256'],
            'COTURN_COMMIT': lock['coturn']['commit'], 'COTURN_SHA256': lock['coturn']['archive_sha256']}
    cmd = ['docker', 'build', '--progress=plain', '-t', IMAGE]
    for key, value in args.items():
        cmd += ['--build-arg', f'{key}={value}']
    return cmd + [str(SERVICES)]


def compose_document():
    common = {'image': IMAGE, 'init': True, 'read_only': True, 'tmpfs': ['/tmp'],
              'cap_drop': ['ALL'], 'security_opt': ['no-new-privileges:true'], 'networks': ['fixture'],
              'mem_limit': '512m', 'cpus': 2, 'pids_limit': 128,
              'ulimits': {'nofile': {'soft': 4096, 'hard': 4096}}}
    return {'name': 'simurghrtc-sr005', 'services': {
        'janus': dict(common, command=['janus'], secrets=['janus_api_secret'],
                      ports=['127.0.0.1:18088:8088', '127.0.0.1:18188:8188']),
        'turn': dict(common, command=['turn'], secrets=['turn_shared_secret']),
        'smoke': dict(common, command=['smoke'], profiles=['test'], secrets=['janus_api_secret', 'turn_shared_secret'])},
        'networks': {'fixture': {'internal': True}},
        'secrets': {name: {'file': f'./secrets/{name}'} for name in ['janus_api_secret', 'turn_shared_secret']}}


def initialize(runtime: Path):
    if runtime.exists() and any(runtime.iterdir()):
        raise ValueError('Runtime directory must be new or empty; credentials are never overwritten.')
    # Validate both inputs before writing any files. Values never enter Compose or process arguments.
    values = {}
    for name, variable in [('janus_api_secret', 'SIMURGHRTC_FIXTURE_JANUS_SECRET'),
                           ('turn_shared_secret', 'SIMURGHRTC_FIXTURE_TURN_SECRET')]:
        value = os.environ.get(variable) or secrets.token_hex(32)
        if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', value):
            raise ValueError(f'{variable} must contain 32-128 safe ASCII characters')
        values[name] = value
    runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
    runtime.chmod(0o700)
    (runtime / 'secrets').mkdir(mode=0o700)
    for name, value in values.items():
        path = runtime / 'secrets' / name
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as output:
            output.write(value + '\n')
    (runtime / 'compose.json').write_text(json.dumps(compose_document(), indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['build', 'init', 'up', 'check', 'down', 'status'])
    parser.add_argument('--runtime', type=Path, default=ROOT / 'LocalBuilds/Fixtures')
    parser.add_argument('--output', type=Path, help='New evidence directory; required for check.')
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    if args.action == 'build':
        return subprocess.call(build_command())
    if args.action == 'init':
        initialize(runtime)
        print(f'Created {runtime / "compose.json"}; credentials remain in protected local files.')
        return 0
    if not (runtime / 'compose.json').is_file():
        parser.error('Initialize a runtime directory first.')
    compose = ['docker', 'compose', '-f', str(runtime / 'compose.json')]
    if args.action == 'check':
        if args.output is None:
            parser.error('check requires --output.')
        output = args.output.resolve()
        if output.exists() and any(output.iterdir()):
            parser.error('Evidence directory must be new or empty.')
        output.mkdir(parents=True, exist_ok=True)
        image_id = subprocess.check_output(['docker', 'image', 'inspect', IMAGE, '--format', '{{.Id}}'], text=True).strip()
        # Reject a stale running service even if the mutable local tag was rebuilt.
        container_ids = subprocess.check_output(compose + ['ps', '-q', 'janus', 'turn'], text=True).split()
        if len(container_ids) != 2:
            raise ValueError('Start both fixture services before checking.')
        for container in container_ids:
            running_image = subprocess.check_output(['docker', 'inspect', container, '--format', '{{.Image}}'], text=True).strip()
            if running_image != image_id:
                raise ValueError('Fixture image changed: run up again to recreate both services.')
        container_name = 'simurghrtc-smoke-' + uuid.uuid4().hex
        try:
            result = subprocess.run(compose + ['run', '--name', container_name, '--rm', '--no-deps', '-T', 'smoke'],
                                    capture_output=True, text=True, timeout=150)
            stdout, stderr, exit_code = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            def decode(value):
                return value.decode(errors='replace') if isinstance(value, bytes) else (value or '')
            stdout, stderr, exit_code = decode(error.stdout), decode(error.stderr) + '\nFixture deadline exceeded.\n', 124
        finally:
            # Killing the Docker CLI does not stop its container. Only remove this run's unique name.
            subprocess.run(['docker', 'rm', '-f', container_name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        # Smoke output never contains secrets, authorization payloads or credential-bearing URLs.
        (output / 'process.log').write_text(stdout + stderr)
        record = validate_report(stdout, exit_code, json.loads(LOCK.read_text()), source_hashes(SERVICES))
        record['image_id'] = image_id
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        # Archive the exact distribution package inventory in this image.
        inventory = subprocess.check_output(['docker', 'run', '--rm', '--network', 'none', '--entrypoint', 'cat', image_id,
                                             '/opt/fixture/debian-packages.txt'], text=True)
        (output / 'debian-packages.txt').write_text(inventory)
        print(json.dumps({'passed': record['passed'], 'evidence': str(output / 'result.json')}))
        return 0 if record['passed'] else 1
    operations = {'up': ['up', '-d', 'janus', 'turn'], 'down': ['down', '--timeout', '10'], 'status': ['ps']}
    return subprocess.call(compose + operations[args.action])


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
