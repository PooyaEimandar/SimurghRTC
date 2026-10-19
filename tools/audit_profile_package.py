#!/usr/bin/env python3
"""Check a Mac ARM64 package against its selected modules, UHT inputs and runtime report."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from prepare_profile import POLICY, documents
from run_lifecycle_probe import CLIENT_PROFILES, validate_report


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def check_modules(objects: str, uht: dict, profile: str) -> list[str]:
    policy = json.loads(POLICY.read_text())
    expected = {name for name in policy['profiles'][profile]
                if 'Game' not in policy['modules'][name].get('deny_targets', [])
                and 'Game' in policy['modules'][name].get('targets', ['Game'])}
    linked = set(re.findall(r'/Development/(SimurghRTC\w+)/', objects))
    if linked != expected:
        raise ValueError(f'Linked modules {sorted(linked)} differ from {sorted(expected)}.')
    reflected = {entry['Name'] for entry in uht['Modules'] if entry['Name'].startswith('SimurghRTC')}
    expected_reflection = expected & {'SimurghRTCCore', 'SimurghRTCMedia', 'SimurghRTCJanusExtensions'}
    if reflected != expected_reflection:
        raise ValueError('UHT includes the wrong reflected modules.')
    if '/ContractHostEditor/' in objects or any(entry['Name'] == 'ContractHostEditor' for entry in uht['Modules']):
        raise ValueError('The Blueprint generator leaked into the Game target.')
    archives = set(re.findall(r'\] (.*[Ww]eb[Rr][Tt][Cc].*?\.a)\(', objects))
    if len(archives) != 1:
        raise ValueError(f'Expected one linked libwebrtc archive; found {sorted(archives)}.')
    return sorted(expected)


def audit(project: Path, profile: str, runtime: Path) -> dict:
    plugin = project / 'Plugins/SimurghRTC'
    actual = tuple(json.loads((plugin / name).read_text()) for name in ('SimurghRTC.uplugin', 'SimurghRTC.profile.json'))
    if actual != documents(profile):
        raise ValueError('Staged descriptor or manifest differs from current profile policy.')
    if {entry.name for entry in (plugin / 'Source').iterdir()} != set(actual[1]['modules']):
        raise ValueError('Staged source includes the wrong modules.')
    mapfile = project / 'Binaries/Mac/ContractHost.map'
    lines = []
    with mapfile.open('rb') as stream:
        for line in stream:
            if line.startswith(b'# Sections:'):
                break
            lines.append(line.decode('utf-8'))
    objects = ''.join(lines)
    native = project / 'Binaries/Mac/ContractHost'
    if f'# Path: {native}\n' not in objects or '# Arch: arm64\n' not in objects:
        raise ValueError('Link map belongs to a different binary or architecture.')
    uht_path = project / 'Intermediate/Build/Mac/ContractHost/ContractHost.uhtmanifest'
    modules = check_modules(objects, json.loads(uht_path.read_text()), profile)
    executable = project / 'Saved/StagedBuilds/Mac/ContractHost.app/Contents/MacOS/ContractHost'
    if subprocess.check_output(['xcrun', 'lipo', '-archs', str(executable)], text=True).strip() != 'arm64':
        raise ValueError('Expected a thin ARM64 packaged executable.')
    uuids = re.findall(r'UUID: ([A-F0-9-]+) \(arm64\)', subprocess.check_output(
        ['xcrun', 'dwarfdump', '--uuid', str(native), str(executable)], text=True))
    if len(uuids) != 2 or uuids[0] != uuids[1]:
        raise ValueError('Native and packaged executable UUIDs differ.')
    result = json.loads(runtime.read_text())
    if result.get('passed') is not True or result.get('process_stopped') is not True or result.get('exit_code') != 0:
        raise ValueError('Packaged runtime probe did not finish cleanly.')
    if result.get('kind') != 'game' or result.get('profile') != profile:
        raise ValueError('Runtime evidence belongs to a different target/profile.')
    report = result['lifecycle']
    validate_report(report, result['run_id'], report['pid'], False, profile, report['provider'])
    binary_hash = sha256(executable)
    if result['executable_sha256'] != binary_hash:
        raise ValueError('Runtime evidence belongs to a different executable.')
    optional = project / 'Content/Examples/Janus/BP_JanusProfile.uasset'
    if optional.is_file() != actual[1]['with_janus_extensions']:
        raise ValueError('Optional source asset selection differs from the profile.')
    return {'schema': 1, 'passed': True, 'profile': profile, 'platform': 'Mac', 'architecture': 'arm64',
            'modules': modules, 'native_and_packaged_uuid': uuids[0], 'executable_sha256': binary_hash,
            'executable_bytes': executable.stat().st_size, 'map_sha256': sha256(mapfile),
            'uht_manifest_sha256': sha256(uht_path), 'runtime_result': str(runtime),
            'janus_blueprint': report['janus_blueprint'], 'network_transport_tested': False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-dir', required=True, type=Path)
    parser.add_argument('--profile', required=True, choices=CLIENT_PROFILES)
    parser.add_argument('--runtime-result', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output must be a new file.')
    result = audit(args.project_dir.resolve(), args.profile, args.runtime_result.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
