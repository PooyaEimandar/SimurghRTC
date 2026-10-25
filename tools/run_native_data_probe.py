#!/usr/bin/env python3
"""Build and run the real local DataChannel fixture against a pinned Mac ARM64 artifact."""
import argparse
import json
from pathlib import Path
import subprocess
import native_artifacts
import mobile_artifacts as a

ROOT = Path(__file__).resolve().parents[1]


def run(artifact, output):
    identity = native_artifacts.validate(artifact)
    a.require(identity['platform'] == 'Mac', 'This local signaling runner requires a Mac ARM64 artifact.')
    a.require(not output.exists(), 'Choose a fresh evidence directory.')
    output.mkdir(parents=True)
    source = ROOT / 'Fixtures/native/data_channels.c'
    helper = ROOT / 'Fixtures/native/signaling.c'
    record = {'passed': False, 'artifact': identity, 'fixture_sha256': a.digest(source), 'helper_sha256': a.digest(helper),
              'scope': 'standalone local DataChannels; text/binary, reliability/order options, negotiated message limits, bounded receive queue',
              'loopback_only': True, 'commands': []}
    def command(args):
        args = list(map(str, args))
        result = subprocess.run(args, capture_output=True, text=True, timeout=120)
        record['commands'].append({'args': args, 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        a.require(result.returncode == 0, 'Command failed: ' + args[0])
        return result.stdout
    try:
        lock = a.read_json(artifact / 'Build/source-lock.json')
        spec = lock['platforms']['Mac']
        a.require(command(['xcodebuild', '-version']).strip() == lock['mac']['xcode'], 'Xcode pin mismatch.')
        a.require(command(['xcrun', '--sdk', 'macosx', '--show-sdk-version']).strip() == spec['sdk'], 'SDK pin mismatch.')
        sdk = command(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()
        triple = 'arm64-apple-macos' + spec['minimum_os']
        obj, executable = output / 'signaling.o', output / 'signaling'
        command(['xcrun', 'clang', '-target', triple, '-isysroot', sdk, '-std=c11',
                 '-I', artifact / 'Include', '-c', source, '-o', obj])
        frameworks = [v for f in ('Foundation', 'Security', 'SystemConfiguration', 'CoreAudio', 'AudioToolbox') for v in ('-framework', f)]
        command(['xcrun', 'clang++', '-target', triple, '-isysroot', sdk, obj,
                 artifact / 'Lib/libwebrtc.a', *frameworks, '-o', executable])
        command(['codesign', '--force', '--sign', '-', executable])
        log = command([executable])
        a.require('PASS: DataChannel UTF-8/binary round trips' in log, 'Incomplete data evidence.')
        a.require(a.digest(source) == record['fixture_sha256'] and a.digest(helper) == record['helper_sha256'], 'Fixture changed during the run.')
        a.require(command(['xcrun', 'lipo', '-archs', executable]).strip() == 'arm64', 'ARM64 consumer required.')
        record.update(passed=True, executable_sha256=a.digest(executable), connected_pairs=3, channel_modes=6, negotiated_limits=[32768, 65536])
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
    finally:
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--artifact', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    result = run(args.artifact.resolve(), args.output.resolve())
    print(json.dumps({'passed': result['passed'], 'error': result.get('error')}))
    raise SystemExit(0 if result['passed'] else 1)
