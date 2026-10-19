#!/usr/bin/env python3
"""Link a C consumer against an isolated artifact and run its lifecycle/codec checks."""
import argparse
import json
from pathlib import Path
import platform
import re
import subprocess
import uuid

import mobile_artifacts as a
import native_artifacts
from run_mobile_probe import ios_simulator, ios_simulator_runtime

ROOT = Path(__file__).resolve().parents[1]


def run_probe(artifact, output, device=None, ndk=None, adb=None):
    a.require(not output.exists(), 'Use a new evidence directory.')
    identity = native_artifacts.validate(artifact)
    target = identity['platform']
    a.require(target in ('Mac', 'IOSSimulator', 'Android'), 'This runner does not support physical iOS.')
    a.require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Run this consumer harness on Mac ARM64.')
    a.require(target == 'Mac' or device, 'Select a device serial or simulator UDID.')
    source = ROOT / 'Config/NativeBackend/abi_test.c'
    build = a.read_json(artifact / 'Build/build.json')
    a.require(a.digest(source) == build['bridge_sources']['abi_test.c'], 'C ABI fixture changed after artifact build.')
    output.mkdir(parents=True)
    record = {'schema': 1, 'passed': False, 'artifact': identity, 'fixture_sha256': a.digest(source),
              'scope': 'standalone C ABI lifecycle and local codecs; not a packaged Unreal run'}
    commands, remote = [], None
    adb_args = [str(adb), '-s', device] if target == 'Android' else []

    def command(args, check=True, timeout=120):
        args = [str(arg) for arg in args]
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        commands.append({'command': args, 'exit_code': result.returncode,
                         'stdout': result.stdout, 'stderr': result.stderr})
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        a.require(not check or result.returncode == 0, 'Command failed; see commands.json: ' + args[0])
        return result

    try:
        executable = output / 'abi_test'
        obj = output / 'abi_test.o'
        archive = artifact / 'Lib/libwebrtc.a'
        if target == 'Android':
            a.require(ndk is not None and 'Pkg.Revision = 27.2.12479018' in (ndk / 'source.properties').read_text(),
                      'Android consumer requires NDK r27c.')
            tools = ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin'
            a.require('clang version 18.0.3' in command([tools / 'clang', '--version']).stdout, 'NDK compiler drift.')
            command([tools / 'clang', '--target=aarch64-none-linux-android26', '-std=c11', '-I', artifact / 'Include',
                     '-c', source, '-o', obj])
            command([tools / 'clang++', '--target=aarch64-none-linux-android26', '-static-libstdc++', obj, archive,
                     '-Wl,-Bsymbolic', '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384', '-llog', '-landroid', '-lm', '-o', executable])
            headers = command([tools / 'llvm-readelf', '-lhW', executable]).stdout
            (output / 'elf.txt').write_text(headers)
            loads = [line.split() for line in headers.splitlines() if line.strip().startswith('LOAD')]
            a.require('AArch64' in headers and loads and all(int(line[-1], 16) >= 16384 for line in loads),
                      'Consumer must be ARM64 with 16 KiB ELF load alignment.')
            record['minimum_load_alignment'] = min(int(line[-1], 16) for line in loads)
            symbols = command([tools / 'llvm-nm', '--defined-only', '--extern-only', '--format=posix', archive]).stdout
            exports = {line.split()[0] for line in symbols.splitlines() if line.strip() and not line.endswith(':')}
            a.require(exports == set(build['exports']), 'Unexpected Android exports.')
            a.require(not device.startswith('emulator-') and command(adb_args + ['get-state']).stdout.strip() == 'device',
                      'Select an authorized physical Android device.')
            a.require(command(adb_args + ['shell', 'getprop', 'ro.kernel.qemu']).stdout.strip() != '1', 'Emulator rejected.')
            a.require('arm64-v8a' in command(adb_args + ['shell', 'getprop', 'ro.product.cpu.abilist']).stdout,
                      'ARM64 Android required.')
            record['device'] = {'model': command(adb_args + ['shell', 'getprop', 'ro.product.model']).stdout.strip(),
                                'os': command(adb_args + ['shell', 'getprop', 'ro.build.version.release']).stdout.strip()}
            remote = '/data/local/tmp/simurghrtc-abi-' + str(uuid.uuid4())
            command(adb_args + ['push', executable, remote])
            command(adb_args + ['shell', 'chmod', '700', remote])
            result = command(adb_args + ['shell', remote], check=False)
        else:
            lock = a.read_json(artifact / 'Build/source-lock.json')
            spec = lock['platforms'][target]
            a.require(command(['xcodebuild', '-version']).stdout.strip() == lock['mac']['xcode'], 'Xcode pin mismatch.')
            a.require(command(['xcrun', '--sdk', spec['sdk_name'], '--show-sdk-version']).stdout.strip() == spec['sdk'],
                      'Apple SDK pin mismatch.')
            sdk = command(['xcrun', '--sdk', spec['sdk_name'], '--show-sdk-path']).stdout.strip()
            triple = ('arm64-apple-macos' + spec['minimum_os']) if target == 'Mac' else ('arm64-apple-ios' + spec['minimum_os'] + '-simulator')
            command(['xcrun', 'clang', '-target', triple, '-isysroot', sdk, '-std=c11', '-I', artifact / 'Include',
                     '-c', source, '-o', obj])
            frameworks = [arg for name in ('Foundation', 'Security', 'SystemConfiguration', 'CoreAudio', 'AudioToolbox')
                          for arg in ('-framework', name)]
            command(['xcrun', 'clang++', '-target', triple, '-isysroot', sdk, obj, archive, *frameworks, '-o', executable])
            symbols = command(['xcrun', 'nm', '-g', '-U', archive]).stdout
            exports = {line.split()[-1] for line in symbols.splitlines() if line.strip() and not line.endswith(':')}
            a.require(exports == set(build['exports']), 'Unexpected Apple exports.')
            a.require(command(['xcrun', 'lipo', '-archs', executable]).stdout.strip() == 'arm64', 'ARM64-only consumer required.')
            headers = command(['xcrun', 'vtool', '-show-build', executable]).stdout
            (output / 'macho.txt').write_text(headers)
            expected = 'MACOS' if target == 'Mac' else 'IOSSIMULATOR'
            a.require(re.search(r'platform\s+' + expected + r'\b', headers), 'Incorrect Mach-O target platform.')
            command(['codesign', '--force', '--sign', '-', executable])
            if target == 'IOSSimulator':
                record['device'] = ios_simulator(json.loads(command(['xcrun', 'simctl', 'list', 'devices', 'available', '--json']).stdout), device)
                record['runtime'] = ios_simulator_runtime(json.loads(command(['xcrun', 'simctl', 'list', 'runtimes', '--json']).stdout), record['device']['runtime'])
                command(['xcrun', 'simctl', 'bootstatus', device, '-b'])
                result = command(['xcrun', 'simctl', 'spawn', device, executable], check=False)
            else:
                result = command([executable], check=False)
        text = result.stdout + result.stderr
        (output / 'runtime.log').write_text(text)
        a.require(result.returncode == 0 and 'PASS: six peers, overlapping owners, ABI rejection, repeated VP8/Opus;' in text,
                  'C ABI runtime checks did not complete.')
        a.require(text.count('media: 12 frames, 50 packets,') == 2
                  and 'M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d' in text, 'Incomplete media/backend evidence.')
        record.update(passed=True, exit_code=result.returncode, executable_sha256=a.digest(executable),
                      exported_symbols=sorted(exports), peers_created=6, codec_runs=2)
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
    finally:
        if remote:
            try:
                command(adb_args + ['shell', 'rm', '-f', remote])
                absent = command(adb_args + ['shell', 'test', '!', '-e', remote], check=False)
                a.require(absent.returncode == 0, 'Temporary Android consumer remains on device.')
                record['remote_binary_removed'] = True
            except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                record.update(passed=False, cleanup_error=str(error))
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--device')
    parser.add_argument('--ndk', type=Path, default=Path.home() / 'Library/Android/sdk/ndk/27.2.12479018')
    parser.add_argument('--adb', type=Path, default=Path.home() / 'Library/Android/sdk/platform-tools/adb')
    args = parser.parse_args()
    result = run_probe(args.artifact.resolve(), args.output.resolve(), args.device, args.ndk, args.adb)
    print(json.dumps({'passed': result['passed'], 'report': str(args.output / 'result.json'), 'error': result.get('error')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
