#!/usr/bin/env python3
"""Launch a bounded headless Unreal probe, verify real HTTP responses, and save evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import time
import urllib.error
import urllib.request
import uuid

BACKEND_VERSION = 'M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d'


def read_runtime_log(output: Path) -> tuple[str, str]:
    # Packaged Mac Server can use stdout without creating the requested ABSLOG file.
    path = output / 'unreal.log'
    if not path.is_file():
        path = output / 'process.log'
    return path.name, path.read_text(errors='replace')


def validate_health(health: dict, run_id: str, server_target: bool, data_only: bool,
                    artifact_sha256: str | None = None) -> None:
    expected = {
        'status': 'ready', 'run_id': run_id, 'compiled_server_target': server_target,
        'dedicated_runtime': True, 'can_ever_render': False, 'factory_ready': True,
        'peer_connection_ready': True, 'rtc_session_connected': False,
        'factory_mode': 'data-only' if data_only else 'builtin-media',
        'dummy_audio_device': not data_only, 'audio_processing': not data_only,
    }
    for key, value in expected.items():
        if health.get(key) != value:
            raise ValueError(f'Unexpected {key}: {health.get(key)!r}; expected {value!r}')
    if artifact_sha256 is not None:
        if health.get('artifact_manifest_sha256') != artifact_sha256 or health.get('webrtc_adapter') != BACKEND_VERSION:
            raise ValueError('HTTP health does not identify the selected isolated backend.')
    if not health.get('engine_version', '').startswith('5.8.3-'):
        raise ValueError('Probe did not run with Unreal Engine 5.8.3')
    if data_only:
        if health.get('audio_codecs') != [] or health.get('video_codecs') != []:
            raise ValueError('Data-only factory advertised media codecs')
    elif 'opus' not in health.get('audio_codecs', []) or not health.get('video_codecs'):
        raise ValueError('Builtin media factory did not advertise Opus and video codecs')


def validate_native_log(log: str, artifact_sha256: str, server_target: bool, data_only: bool) -> None:
    # Also proves native startup identity when a bind failure prevents HTTP health.
    expected = (f'SIMURGHRTC_PROBE_NATIVE backend={BACKEND_VERSION} artifact={artifact_sha256} '
                f'compiled_server={int(server_target)} mode={"data-only" if data_only else "builtin-media"}')
    if not any(line.rstrip().endswith(expected) for line in log.splitlines()):
        raise ValueError('Runtime log does not identify the selected native backend and target.')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path,
                        help='UnrealEditor-Cmd or the staged ContractHostServer executable, not an .app directory.')
    parser.add_argument('--kind', required=True, choices=['editor-server', 'server'])
    parser.add_argument('--project', type=Path, help='Required for editor-server; omit for a staged Server executable.')
    parser.add_argument('--output', required=True, type=Path, help='New or empty evidence directory.')
    parser.add_argument('--data-only', action='store_true')
    parser.add_argument('--occupied-port', action='store_true', help='Negative test: require explicit HTTP bind failure and cleanup.')
    parser.add_argument('--native-artifact', type=Path, help='Require this isolated artifact in HTTP health and native startup logs.')
    parser.add_argument('--startup-timeout', type=float, default=180)
    args = parser.parse_args()
    executable = args.executable.resolve()
    output = args.output.resolve()
    if not executable.is_file():
        parser.error('Executable must exist and be a file.')
    if args.kind == 'editor-server' and (not args.project or not args.project.is_file()):
        parser.error('editor-server requires an existing --project.')
    if args.startup_timeout <= 0:
        parser.error('Startup timeout must be positive.')
    if output.exists() and any(output.iterdir()):
        parser.error('Evidence directory must be new or empty; existing evidence is never overwritten.')
    identity = None
    if args.native_artifact:
        import native_artifacts
        identity = native_artifacts.validate(args.native_artifact.resolve())
    output.mkdir(parents=True, exist_ok=True)
    run_id = str(uuid.uuid4())
    reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    reservation.bind(('127.0.0.1', 0))
    reservation.listen(1)
    port = reservation.getsockname()[1]
    command = [str(executable)]
    if args.project:
        command.append(str(args.project.resolve()))
    command += ['/Engine/Maps/Entry', '-server', '-NullRHI', '-nosound', '-unattended', '-nosplash',
                '-SkipZenStore', '-SimurghRTCProbe', f'-SimurghRTCProbePort={port}',
                '-SimurghRTCProbeSeconds=8', f'-SimurghRTCProbeRunId={run_id}',
                f'-abslog={output / "unreal.log"}']
    if args.data_only:
        command.append('-SimurghRTCProbeDataOnly')
    record = {'schema': 1, 'command': command, 'scenario': 'occupied-port' if args.occupied_port else 'ready',
              'kind': args.kind, 'run_id': run_id, 'port': port, 'passed': False,
              'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest()}
    if identity:
        record['native_artifact'] = identity
    process = None
    started = time.monotonic()
    try:
        if not args.occupied_port:
            # Another process can race for the released port; fail-on-bind and run ID validation detect it.
            reservation.close()
        with (output / 'process.log').open('w') as console:
            process = subprocess.Popen(command, cwd=executable.parent, stdout=console, stderr=subprocess.STDOUT)
            if args.occupied_port:
                code = process.wait(timeout=args.startup_timeout)
                record['exit_code'] = code
                record['runtime_log'], log = read_runtime_log(output)
                # UE 5.8 Mac inherits the generic RequestExitWithStatus implementation,
                # which logs and ignores the requested code during graceful shutdown.
                ignored_status = (code == 0 and 'RequestExitWithStatus(0, 2)' in log
                                  and 'return code will be ignored by the generic implementation' in log)
                record['engine_ignored_requested_exit_status'] = ignored_status
                if (code != 2 and not ignored_status) or 'HTTPServer could not bind the probe port' not in log:
                    raise ValueError(f'Expected explicit HTTP bind failure (requested exit 2), got {code}')
                if 'SIMURGHRTC_PROBE_READY' in log:
                    raise ValueError('Probe reported readiness despite an occupied port')
                if 'SIMURGHRTC_PROBE_STOPPED requests=0' not in log:
                    raise ValueError('Bind failure did not clean up the native runtime')
            else:
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                base = f'http://127.0.0.1:{port}'
                deadline = time.monotonic() + args.startup_timeout
                health = None
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise ValueError(f'Probe exited before HTTP readiness (exit {process.returncode})')
                    try:
                        with opener.open(base + '/simurghrtc/probe/health', timeout=1) as response:
                            if response.status != 200 or response.headers.get_content_type() != 'application/json':
                                raise ValueError('Unexpected HTTP status or content type')
                            health = json.load(response)
                        break
                    except (urllib.error.URLError, TimeoutError, ConnectionError):
                        time.sleep(0.25)
                if health is None:
                    raise TimeoutError('Probe did not become HTTP ready within the startup deadline')
                validate_health(health, run_id, args.kind == 'server', args.data_only,
                                identity['manifest_sha256'] if identity else None)
                if health.get('pid') != process.pid:
                    raise ValueError('HTTP health response did not come from the launched process')
                record['health'] = health
                # Exercise routing, not just a log line or a raw TCP accept.
                for path, method in [('/does-not-exist', 'GET'), ('/simurghrtc/probe/health', 'POST')]:
                    request = urllib.request.Request(base + path, method=method)
                    try:
                        opener.open(request, timeout=2).close()
                        raise ValueError(f'{method} {path} unexpectedly succeeded')
                    except urllib.error.HTTPError as error:
                        if error.code not in (404, 405):
                            raise ValueError(f'Unexpected rejection status {error.code}') from error
                code = process.wait(timeout=30)
                record['exit_code'] = code
                record['runtime_log'], log = read_runtime_log(output)
                if code != 0 or 'SIMURGHRTC_PROBE_STOPPED' not in log:
                    raise ValueError(f'Probe did not shut down cleanly (exit {code})')
                with socket.socket() as connection:
                    connection.settimeout(1)
                    if connection.connect_ex(('127.0.0.1', port)) == 0:
                        raise ValueError('Probe port still accepts connections after shutdown')
            if identity:
                validate_native_log(log, identity['manifest_sha256'], args.kind == 'server', args.data_only)
                record['native_start_verified'] = True
            record['passed'] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
    finally:
        reservation.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'kind': args.kind, 'scenario': record['scenario'],
                      'evidence': str(output / 'result.json'), 'error': record.get('error')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
