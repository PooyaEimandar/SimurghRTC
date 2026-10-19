#!/usr/bin/env python3
"""Run the native codec + UE texture/render-target/audio-mixer probe and retain evidence."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import time
import uuid

from run_native_probe import read_runtime_log


def read_media_report(output: Path, log: str) -> tuple[dict, str]:
    path = output / 'media.json'
    if path.is_file():
        return json.loads(path.read_text()), 'file'
    marker = 'SIMURGHRTC_MEDIA_REPORT '
    reports = [json.loads(line.split(marker, 1)[1]) for line in log.splitlines() if marker in line]
    if len(reports) != 1:
        raise ValueError('Expected exactly one structured media report in the runtime log')
    return reports[0], 'runtime_log'


def validate_report(report: dict, run_id: str, pid: int, editor: bool, negative: str | None = None, artifact_sha256: str | None = None) -> None:
    expected = {'run_id': run_id, 'pid': pid, 'editor_build': editor,
                'network_transport_tested': False, 'resources_released': True, 'passed': negative is None}
    for key, value in expected.items():
        if report.get(key) != value:
            raise ValueError(f'Unexpected {key}: {report.get(key)!r}; expected {value!r}')
    if artifact_sha256 is not None:
        if report.get('artifact_manifest_sha256') != artifact_sha256 or report.get('native_backend') != 'M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d':
            raise ValueError('Media report does not identify the selected isolated backend.')
    if not report.get('engine_version', '').startswith('5.8.3-'):
        raise ValueError('Probe requires Unreal Engine 5.8.3')
    if negative:
        message = 'requires a real RHI' if negative == 'null-rhi' else 'audio device/component unavailable'
        if message not in report.get('error', ''):
            raise ValueError(f'Expected explicit {negative} rejection, got {report.get("error")!r}')
        return
    expected = {'video_codec': 'VP8', 'audio_codec': 'opus', 'encoded_frames': 12, 'decoded_frames': 12,
                'audio_packets': 50, 'pcm_sample_rate': 48000, 'pcm_channels': 2, 'pcm_samples': 96000,
                'texture_uploads': 2, 'render_target_captures': 2, 'frame_bytes': 16384,
                'max_in_flight_frames': 1, 'pcm_queue_limit_bytes': 192000,
                'pcm_queued_bytes': 192000, 'pcm_consumed_bytes': 192000,
                'audio_callback_on_game_thread': False, 'error': ''}
    for key, value in expected.items():
        if report.get(key) != value:
            raise ValueError(f'Unexpected {key}: {report.get(key)!r}; expected {value!r}')
    bounds = {'tone_correlation': (0.95, 1.001), 'pcm_rms': (3000, 9000),
              'codec_max_color_error': (0, 12), 'capture_max_color_error': (0, 3),
              'max_video_packet_bytes': (1, 65536), 'max_audio_packet_bytes': (1, 4096),
              'audio_callbacks': (1, 10000)}
    for key, (low, high) in bounds.items():
        value = report.get(key)
        if not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError(f'Invalid {key}: {value!r}')
    if not report.get('rhi') or 'null' in report['rhi'].lower():
        raise ValueError('Real RHI required for render-target evidence')
    if not report.get('video_encoder') or not report.get('video_decoder'):
        raise ValueError('Codec implementation identity missing')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['editor-game', 'game'])
    parser.add_argument('--project', type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New or empty evidence directory.')
    parser.add_argument('--native-artifact', type=Path, help='Require this isolated backend identity in the report.')
    parser.add_argument('--negative', choices=['null-rhi', 'no-audio'])
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    executable, output = args.executable.resolve(), args.output.resolve()
    if not executable.is_file():
        parser.error('Executable must exist.')
    if args.kind == 'editor-game' and (not args.project or not args.project.is_file()):
        parser.error('editor-game requires an existing --project.')
    if args.kind == 'game' and args.project:
        parser.error('Packaged game uses its staged project; omit --project.')
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('Timeout must be finite and positive.')
    if output.exists() and any(output.iterdir()):
        parser.error('Output must be new or empty; evidence is never overwritten.')
    output.mkdir(parents=True, exist_ok=True)
    run_id = str(uuid.uuid4())
    command = [str(executable)]
    if args.project:
        command.append(str(args.project.resolve()))
    command += ['/Engine/Maps/Entry', '-game', '-RenderOffscreen', '-unattended', '-nosplash', '-SkipZenStore',
                '-windowed', '-ResX=320', '-ResY=240', '-SimurghRTCMediaProbe',
                f'-SimurghRTCMediaRunId={run_id}', f'-SimurghRTCMediaOutput={output / "media.json"}',
                f'-abslog={output / "unreal.log"}']
    if args.negative:
        command.append('-NullRHI' if args.negative == 'null-rhi' else '-nosound')
    record = {'schema': 1, 'command': command, 'kind': args.kind, 'scenario': args.negative or 'media',
              'run_id': run_id, 'passed': False}
    artifact_sha256 = None
    if args.native_artifact:
        import native_artifacts
        artifact_sha256 = native_artifacts.validate(args.native_artifact.resolve())['manifest_sha256']
        record['artifact_manifest_sha256'] = artifact_sha256
    started, process = time.monotonic(), None
    try:
        with (output / 'process.log').open('w') as console:
            process = subprocess.Popen(command, cwd=executable.parent, stdout=console, stderr=subprocess.STDOUT)
            record['exit_code'] = process.wait(timeout=args.timeout)
        record['runtime_log'], log = read_runtime_log(output)
        report, record['report_source'] = read_media_report(output, log)
        record['media'] = report
        validate_report(report, run_id, process.pid, args.kind == 'editor-game', args.negative, artifact_sha256)
        if record['report_source'] == 'runtime_log':
            (output / 'media.json').write_text(json.dumps(report, indent=2) + '\n')
        marker = 'FAILED' if args.negative else 'PASSED'
        if f'SIMURGHRTC_MEDIA_PROBE_{marker}' not in log:
            raise ValueError('Missing probe completion marker')
        if args.negative:
            ignored = (record['exit_code'] == 0 and 'RequestExitWithStatus(0, 4)' in log
                       and 'return code will be ignored by the generic implementation' in log)
            if record['exit_code'] != 4 and not ignored:
                raise ValueError('Unexpected negative-test exit status')
            record['engine_ignored_requested_exit_status'] = ignored
        elif record['exit_code'] != 0:
            raise ValueError('Probe did not exit cleanly')
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
        record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'evidence': str(output / 'result.json'), 'error': record.get('error')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
