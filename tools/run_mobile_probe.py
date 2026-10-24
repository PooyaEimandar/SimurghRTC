#!/usr/bin/env python3
"""Check a packaged lifecycle probe on Android, iOS or an explicit iOS simulator."""
from __future__ import annotations
import argparse
import json
import math
import os
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import time
import uuid
import zipfile

import mobile_artifacts
import ios_simulator_support

MARKER = 'SIMURGHRTC_CLIENT_REPORT '


def require(ok, message):
    if not ok:
        raise ValueError(message)


def validate_report(report, run_id, platform, manifest_sha256, with_janus, *, editor=False, simulator=False, media=False):
    require(isinstance(report, dict), 'Expected a JSON object report.')
    expected = {'schema': 2 if media else 1, 'run_id': run_id, 'platform': platform, 'editor_build': editor,
                'server_build': False, 'arm64_build': True, 'simulator_build': simulator, 'with_janus': with_janus,
                'artifact_manifest_sha256': manifest_sha256, 'network_transport_tested': False,
                'hardware_capture_tested': False, 'completed_cycles': 3, 'factories_created': 9,
                'peers_created': 9, 'explicit_teardowns': 6, 'destructor_teardowns': 3,
                'overlapping_owners_checked': True, 'duplicate_calls_checked': True,
                'readiness_cleared_after_stop': True, 'passed': True, 'error': ''}
    if media:
        expected.update(media_codecs_passed=True, decoded_video_frames=12, decoded_pcm_samples=96000,
                        audio_packets=50, render_capture_tested=False,
                        native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d')
        for key, low, high in [('video_color_error',0,12),('audio_tone_correlation',0.95,1.01)]:
            value = report.get(key)
            require(type(value) in (int,float) and math.isfinite(value) and low <= value <= high, 'Invalid codec measurement: ' + key)
    for key, value in expected.items():
        require(type(report.get(key)) is type(value) and report[key] == value,
                f'Unexpected {key}: {report.get(key)!r}; expected {value!r}')
    require(isinstance(report.get('engine_version'), str) and report['engine_version'].startswith('5.8.3-'),
            'Expected Unreal Engine 5.8.3.')
    require(type(report.get('pid')) is int and report['pid'] > 0, 'Invalid probe process ID.')
    elapsed = report.get('elapsed_seconds')
    require(type(elapsed) in (int, float) and math.isfinite(elapsed) and 0 <= elapsed <= 120,
            'Invalid or excessive lifecycle duration.')


def read_report(log, run_id, marker=MARKER):
    matches = []
    for line in log.splitlines():
        if marker not in line:
            continue
        try:
            report = json.loads(line.split(marker, 1)[1])
        except json.JSONDecodeError:
            continue  # A live log read can end partway through a line.
        if isinstance(report, dict) and report.get('run_id') == run_id:
            matches.append(report)
    require(len(matches) <= 1, 'Duplicate reports for this run ID.')
    return matches[0] if matches else None


def physical_ios_device(document, identifier):
    candidates = [d for d in document['result']['devices'] if d['identifier'] == identifier]
    require(len(candidates) == 1, 'Select one device identifier from devicectl list devices.')
    device = candidates[0]
    hardware = device.get('properties', {}).get('hardware', device.get('hardwareProperties', {}))
    require(hardware.get('reality') == 'physical' and hardware.get('platform') == 'iOS',
            'A physical iPhone/iPad is required; simulators do not establish device support.')
    return device


def legacy_ios_device(connected, identifier, model, version, architecture):
    require(identifier in connected.splitlines(), 'Select a connected USB iOS device UDID.')
    require(re.fullmatch(r'(iPhone|iPad)[0-9]+,[0-9]+', model), 'Expected a physical iPhone/iPad model.')
    require(architecture in ('arm64', 'arm64e'), 'An ARM64 iOS device is required.')
    require(re.fullmatch(r'[0-9]+(\.[0-9]+){1,2}', version), 'Invalid iOS version.')
    return {'model': model, 'os_version': version, 'architecture': architecture}


def ios_simulator(document, identifier):
    matches = [(runtime, device) for runtime, devices in document['devices'].items()
               for device in devices if device.get('udid') == identifier]
    require(len(matches) == 1, 'Select one simulator UDID from simctl list devices.')
    runtime, device = matches[0]
    require(runtime.startswith('com.apple.CoreSimulator.SimRuntime.iOS-') and device.get('isAvailable') is True,
            'An available iOS simulator is required.')
    return dict(device, runtime=runtime)


def ios_simulator_runtime(document, identifier):
    matches = [runtime for runtime in document['runtimes'] if runtime.get('identifier') == identifier]
    require(len(matches) == 1, 'Simulator runtime is missing or ambiguous.')
    runtime = matches[0]
    require(runtime.get('isAvailable') is True and runtime.get('platform') == 'iOS'
            and 'arm64' in runtime.get('supportedArchitectures', []), 'An ARM64 iOS simulator runtime is required.')
    return {key: runtime[key] for key in ['identifier', 'name', 'version', 'buildversion', 'supportedArchitectures']}


def simulator_launch_pid(log, app_id):
    matches = re.findall(r'^' + re.escape(app_id) + r':\s*([0-9]+)\s*$', log, re.MULTILINE)
    require(len(matches) == 1 and int(matches[0]) > 0, 'Missing or ambiguous simulator launch PID.')
    return int(matches[0])


def read_simulator_report(container, run_id, prefix="client"):
    # Unreal logs through Apple's unified log on iOS, not necessarily stdout.
    # Read only this app's saved report, selected by the new run's UUID.
    matches = list(container.rglob(prefix + '-probe-' + run_id + '.json'))
    require(len(matches) <= 1, 'Duplicate saved simulator reports for this run ID.')
    if not matches:
        return None
    try:
        return json.loads(matches[0].read_text())
    except json.JSONDecodeError:
        return None  # The game thread may still be writing this file.


def process_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def read_android_chunks(log, run_id, kind="MEDIA", maximum=16):
    marker = "SIMURGHRTC_" + kind + "_CHUNK " + run_id + " "
    chunks, total = {}, None
    for line in log.splitlines():
        if marker not in line:
            continue
        match = re.fullmatch(r'([0-9]+)/([0-9]+) (.*)', line.split(marker, 1)[1])
        require(match is not None, 'Malformed Android report chunk.')
        index, count, payload = int(match[1]), int(match[2]), match[3]
        require(1 <= index <= count <= maximum and len(payload) <= 640, 'Invalid Android chunk bounds.')
        require(total in (None, count) and index not in chunks, 'Conflicting or duplicate Android chunks.')
        total, chunks[index] = count, payload
    if total is None or len(chunks) != total:
        return None
    try:
        report = json.loads(''.join(chunks[index] for index in range(1, total + 1)))
    except json.JSONDecodeError:
        raise ValueError('Truncated or invalid Android report.') from None
    require(isinstance(report, dict) and report.get('run_id') == run_id,
            'Android report does not match this run.')
    return report


def read_android_media_chunks(log, run_id):
    return read_android_chunks(log, run_id)


def android_package_sizes(app):
    with zipfile.ZipFile(app) as package:
        libraries = [entry for entry in package.infolist() if entry.filename.startswith('lib/') and not entry.is_dir()]
        require(libraries and all(entry.filename.startswith('lib/arm64-v8a/') for entry in libraries),
                'Expected an ARM64-only APK.')
        return {'apk_bytes': app.stat().st_size,
                'native_libraries': {entry.filename: {'uncompressed_bytes': entry.file_size,
                                                     'compressed_bytes': entry.compress_size}
                                     for entry in libraries},
                'shipping_baseline': False, 'rtc_size_delta_measured': False}


def android_main_pid(processes, app_id):
    rows = [line.split() for line in processes.splitlines()[1:] if line.strip()]
    require(rows and all(len(row) == 3 and row[0].isdigit() and row[1].isdigit() and row[2] == app_id
                        for row in rows), 'Unexpected Android app process inventory.')
    pids = {int(row[0]) for row in rows}
    # Helpers may keep the app name, including detached children reparented to init.
    # The ActivityManager-launched app remains a child of Android's zygote.
    roots = [int(row[0]) for row in rows if int(row[1]) > 1 and int(row[1]) not in pids]
    require(len(roots) == 1, 'Expected one root Android app process.')
    return roots[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['Android', 'IOS', 'IOSSimulator'], required=True)
    parser.add_argument('--project', type=Path, required=True, help='Staged directory with mobile-probe.json.')
    parser.add_argument('--app', type=Path, required=True, help='APK for Android; signed .app directory for iOS.')
    parser.add_argument('--device', required=True, help='adb serial, devicectl identifier, iOS USB UDID or simulator UDID.')
    parser.add_argument('--ios-backend', choices=['devicectl', 'libimobiledevice'], default='devicectl',
                        help='Use libimobiledevice for iOS 16, with its developer disk image already mounted.')
    parser.add_argument('--adb', type=Path, default=Path.home() / 'Library/Android/sdk/platform-tools/adb')
    parser.add_argument('--aapt', type=Path, default=Path.home() / 'Library/Android/sdk/build-tools/36.0.0/aapt2')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--probe', choices=['lifecycle', 'media', 'signaling'], default='lifecycle',
                        help='Android media additionally checks render-target capture and silent mixer playback.')
    parser.add_argument('--pairs', type=int, default=8, help='Signaling probe connection pairs (2–32).')
    args = parser.parse_args()
    require(2 <= args.pairs <= 32, 'Use 2–32 signaling pairs.')
    require(args.probe != 'media' or args.platform == 'Android',
            'The mobile render/mixer harness currently supports physical Android devices.')
    require(math.isfinite(args.timeout) and 5 <= args.timeout <= 600, 'Use a timeout from 5 to 600 seconds.')
    output = args.output.resolve()
    require(not output.exists(), 'Use a new evidence directory; previous reports are never overwritten.')
    project, app = args.project.resolve(), args.app.resolve()
    config = mobile_artifacts.read_json(project / 'mobile-probe.json')
    app_id = config['app_id']
    require(re.fullmatch(r'[A-Za-z][A-Za-z0-9]*(\.[A-Za-z][A-Za-z0-9]*){2,}', app_id), 'Invalid app ID.')
    artifact = project / 'Plugins/SimurghRTC/ThirdParty/WebRTC' / args.platform / 'arm64'
    identity = mobile_artifacts.validate(artifact)
    require(identity == config['artifacts'][args.platform], 'Staged dependency changed after probe preparation.')
    require(app.exists(), 'Packaged app is missing.')
    output.mkdir(parents=True)
    record = {'schema': 1, 'passed': False, 'platform': args.platform, 'profile': config['profile'],
              'artifact': identity, 'run_id': str(uuid.uuid4()), 'device': args.device, 'app_id': app_id}
    record['probe_kind'] = args.probe
    simulator = args.platform == 'IOSSimulator'
    record['validation_scope'] = 'simulator' if simulator else 'physical-device'
    run_id, capture, launched, pid = record['run_id'], None, False, None
    simulator_container = None
    started = time.monotonic()
    adb = [str(args.adb), '-s', args.device]
    console = output / 'process.log'

    def command(argv, timeout=45, check=True):
        result = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=timeout)
        with (output / 'commands.log').open('a') as log:
            log.write(json.dumps([str(a) for a in argv]) + '\n' + result.stdout + result.stderr + '\n')
        require(not check or result.returncode == 0, f'{Path(str(argv[0])).name} failed; see commands.log.')
        return result

    try:
        options = ['-SimurghRTCClientProbe', '-SimurghRTCClientRunId=' + run_id, '-nosound',
                   '-stdout', '-FullStdOutLogOutput']
        if args.probe == 'media':
            options = ['-SimurghRTCMediaProbe', '-SimurghRTCMediaRunId=' + run_id,
                       '-unattended', '-stdout', '-FullStdOutLogOutput']
        if args.probe == 'signaling':
            options = ['-SimurghRTCSignalingProbe', '-SimurghRTCSignalingRunId=' + run_id,
                       '-SimurghRTCSignalingPairs=' + str(args.pairs), '-nosound', '-unattended', '-stdout', '-FullStdOutLogOutput']
        if simulator:
            options += ['-nullrhi', '-sm5']  # Matches the probe's headless platform metadata.
        if args.platform == 'Android':
            # adb launches have no stdout reader. Preserve Unreal's logcat sink.
            options = [option for option in options if option not in ('-stdout', '-FullStdOutLogOutput')]
            require(not args.device.startswith('emulator-'), 'A physical Android device is required.')
            require(command(adb + ['get-state']).stdout.strip() == 'device', 'Device is not authorized/online.')
            require(command(adb + ['shell', 'getprop', 'ro.kernel.qemu']).stdout.strip() != '1', 'Emulator rejected.')
            require('arm64-v8a' in command(adb + ['shell', 'getprop', 'ro.product.cpu.abilist']).stdout, 'ARM64 device required.')
            record['device_model'] = command(adb + ['shell', 'getprop', 'ro.product.model']).stdout.strip()
            record['os_version'] = command(adb + ['shell', 'getprop', 'ro.build.version.release']).stdout.strip()
            record['page_size'] = command(adb + ['shell', 'getconf', 'PAGESIZE']).stdout.strip()
            badging = command([args.aapt, 'dump', 'badging', app]).stdout
            require(f"package: name='{app_id}'" in badging, 'APK application ID differs from this probe project.')
            record['app_sha256'] = mobile_artifacts.digest(app)
            record['size'] = android_package_sizes(app)
            command(adb + ['install', '-r', app], timeout=120)
            command(adb + ['shell', 'am', 'force-stop', app_id])
            command(adb + ['shell', 'am', 'start', '-W', '-n', app_id + '/com.epicgames.unreal.GameActivity',
                           '--es', 'cmdline', shlex.quote(' '.join(options))])
            launched = True
            value = command(adb + ['shell', 'pidof', app_id]).stdout.strip().split()
            require(value and all(p.isdigit() for p in value), 'The launched app has no process.')
            processes = command(adb + ['shell', 'ps', '-p', ','.join(value), '-o', 'PID,PPID,NAME']).stdout
            pid = android_main_pid(processes, app_id)
            record['launch_pid'] = pid
        else:
            record['ios_backend'] = 'simctl' if simulator else args.ios_backend
            if simulator:
                support = ios_simulator_support.validate(project / 'Build/IOSSimulator')
                require(support == config.get('simulator_support'), 'Staged simulator support changed after preparation.')
                record['simulator_support'] = support
                inventory = json.loads(command(['xcrun', 'simctl', 'list', 'devices', 'available', '--json']).stdout)
                record['device_info'] = ios_simulator(inventory, args.device)
                runtimes = json.loads(command(['xcrun', 'simctl', 'list', 'runtimes', '--json']).stdout)
                record['runtime_info'] = ios_simulator_runtime(runtimes, record['device_info']['runtime'])
                command(['xcrun', 'simctl', 'bootstatus', args.device, '-b'], timeout=180)
            elif args.ios_backend == 'devicectl':
                info = output / 'devices.json'
                command(['xcrun', 'devicectl', 'list', 'devices', '--json-output', info])
                record['device_info'] = physical_ios_device(json.loads(info.read_text()), args.device)
            else:
                connected = command(['idevice_id', '-l']).stdout
                values = [command(['ideviceinfo', '-u', args.device, '-k', key]).stdout.strip()
                          for key in ['ProductType', 'ProductVersion', 'CPUArchitecture']]
                record['device_info'] = legacy_ios_device(connected, args.device, *values)
            require(app.is_dir() and app.suffix == '.app', 'Provide a signed iOS .app directory.')
            with (app / 'Info.plist').open('rb') as stream:
                plist = plistlib.load(stream)
            record['bundle_toolchain'] = {key: plist[key] for key in ['DTXcode', 'DTXcodeBuild', 'DTSDKName', 'DTSDKBuild']
                                         if key in plist}
            record['host_xcode'] = command(['xcodebuild', '-version']).stdout.strip()
            expected_platform = 'iPhoneSimulator' if simulator else 'iPhoneOS'
            require(plist.get('CFBundleIdentifier') == app_id and expected_platform in plist.get('CFBundleSupportedPlatforms', []),
                    'Expected this probe bundle ID and matching device/simulator app.')
            command(['codesign', '--verify', '--deep', '--strict', app])
            if simulator:
                # A host utility spawned through simctl can itself run under Rosetta.
                # Check the actual app slice, then require arm64_build in its report.
                require(command(['xcrun', 'lipo', '-archs', app / plist['CFBundleExecutable']]).stdout.strip() == 'arm64',
                        'An ARM64-only simulator app is required.')
            if not simulator:
                require((app / 'embedded.mobileprovision').is_file(), 'iOS development provisioning profile is missing.')
            record['executable_sha256'] = mobile_artifacts.digest(app / plist['CFBundleExecutable'])
            record['size'] = {'bundle_file_bytes': sum(p.stat().st_size for p in app.rglob('*') if p.is_file()),
                              'executable_bytes': (app / plist['CFBundleExecutable']).stat().st_size,
                              'shipping_baseline': False, 'rtc_size_delta_measured': False}
            if simulator:
                command(['xcrun', 'simctl', 'install', args.device, app], timeout=120)
                simulator_container = Path(command(['xcrun', 'simctl', 'get_app_container', args.device, app_id, 'data']).stdout.strip())
                require(simulator_container.is_dir(), 'The installed simulator app data container is missing.')
                launch = ['xcrun', 'simctl', 'launch', '--terminate-running-process', args.device,
                          app_id, *options]
            elif args.ios_backend == 'devicectl':
                command(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', args.device, app], timeout=120)
                launch = ['xcrun', 'devicectl', 'device', 'process', 'launch', '--device', args.device,
                          '--terminate-existing', '--console', app_id, '--', *options]
            else:
                command(['ideviceinstaller', '-u', args.device, 'install', app], timeout=120)
                command(['idevicedebug', '-u', args.device, 'kill', app_id], check=False)
                launch = ['idevicedebug', '-u', args.device, 'run', app_id, *options]
            if simulator:
                # Non-console launch exits and flushes the PID immediately.
                # The correlated report comes from this app's data container.
                response = command(launch)
                launched = True
                console.write_text(response.stdout + response.stderr)
                pid = simulator_launch_pid(response.stdout, app_id)
                record['launch_pid'] = pid
            else:
                with console.open('w') as stream:
                    capture = subprocess.Popen(launch, stdout=stream, stderr=subprocess.STDOUT)
            launched = True
        deadline = time.monotonic() + args.timeout
        report = None
        while time.monotonic() < deadline:
            if args.platform == 'Android':
                console.write_text(command(adb + ['logcat', '-d', '-v', 'brief', '--pid=' + str(pid)]).stdout)
            log = console.read_text(errors='replace') if console.exists() else ''
            report = read_report(log, run_id, {'media': 'SIMURGHRTC_MEDIA_REPORT ', 'signaling': 'SIMURGHRTC_SIGNALING_REPORT '}.get(args.probe, MARKER))
            if report is None and args.platform == 'Android' and args.probe in ('media', 'signaling'):
                report = read_android_chunks(log, run_id, args.probe.upper(), 128 if args.probe == 'signaling' else 16)
                if report is not None:
                    record['report_source'] = 'android-log-chunks'
                    (output / 'probe.json').write_text(json.dumps(report, indent=2) + '\n')
            if report is None and simulator_container:
                report = read_simulator_report(simulator_container, run_id, "signaling" if args.probe == "signaling" else "client")
                if report is not None:
                    record['report_source'] = 'simulator-app-container'
                    (output / 'probe.json').write_text(json.dumps(report, indent=2) + '\n')
            if report and (args.probe != 'media' or 'SIMURGHRTC_MEDIA_PROBE_' in log):
                break
            require(args.platform != 'Android' or 'Fatal signal ' not in log,
                    'Android native crash; see process.log and the device crash buffer.')
            if capture and capture.poll() is not None:
                raise ValueError('iOS console exited before a correlated report was received.')
            if simulator:
                require(process_alive(pid), 'Simulator app exited before its correlated report was received.')
            time.sleep(1)
        require(report is not None, 'Timed out waiting for the correlated probe report.')
        record['probe'] = report  # Retain structured failures as well as successes.
        if args.probe == 'media':
            from run_media_probe import validate_report as validate_media
            validate_media(report, run_id, pid, False, artifact_sha256=identity['manifest_sha256'])
            require('SIMURGHRTC_MEDIA_PROBE_PASSED' in log, 'Missing media completion marker.')
        elif args.probe == 'signaling':
            from run_signaling_probe import validate_report as validate_signaling
            validate_signaling(report, run_id, 'IOS' if simulator else args.platform, identity['manifest_sha256'],
                               config['with_janus'], simulator=simulator, pairs=args.pairs)
        else:
            validate_report(report, run_id, 'IOS' if simulator else args.platform, identity['manifest_sha256'],
                            config['with_janus'], simulator=simulator,
                            media=mobile_artifacts.read_json(artifact / 'artifact.json')['metadata']['schema'] == 2)
        if pid is not None:
            require(report['pid'] == pid, 'Report PID differs from the launched app process.')
        pid = report['pid']
        record['passed'] = True
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
    finally:
        try:
            if launched and args.platform == 'Android':
                command(adb + ['shell', 'am', 'force-stop', app_id])
                require(not command(adb + ['shell', 'pidof', app_id], check=False).stdout.strip(), 'App process remains alive.')
            elif launched and simulator:
                command(['xcrun', 'simctl', 'terminate', args.device, app_id])
                cleanup_deadline = time.monotonic() + 20
                if pid is not None:
                    while process_alive(pid) and time.monotonic() < cleanup_deadline:
                        time.sleep(0.1)
                    require(not process_alive(pid), 'Simulator app process remains alive.')
            elif capture:
                if args.ios_backend == 'libimobiledevice':
                    command(['idevicedebug', '-u', args.device, 'kill', app_id])
                elif pid:
                    command(['xcrun', 'devicectl', 'device', 'process', 'terminate', '--device', args.device, '--pid', str(pid)])
                elif capture.poll() is None:
                    capture.terminate()  # devicectl forwards SIGTERM to its launched app.
                capture.wait(timeout=20)
            record['app_stopped'] = launched
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            record['passed'] = False
            record['cleanup_error'] = str(error)
            if capture and capture.poll() is None:
                capture.kill()
                capture.wait(timeout=10)
        record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'evidence': str(output / 'result.json'), 'error': record.get('error')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit(str(error))
