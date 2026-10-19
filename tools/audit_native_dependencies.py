#!/usr/bin/env python3
"""Collect SR-003 dependency evidence. This is an inventory check, not security approval."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

import ios_simulator_support
import mobile_artifacts as artifacts
import mobile_build

ROOT = Path(__file__).resolve().parents[1]
THIRD_PARTY = 'Engine/Source/ThirdParty/'
MAC_LIBRARIES = (
    'WebRTC/5414/Lib/Mac/Release/libwebrtc.a',
    'OpenSSL/1.1.1t/lib/Mac/libssl.a', 'OpenSSL/1.1.1t/lib/Mac/libcrypto.a',
    'libOpus/opus-1.1/Mac/libopus.a', 'libOpus/opus-1.1/Mac/libspeex_resampler.a',
    'libvpx/libvpx-1.14.1/lib/Mac/libvpx.a',
    'libvpx/libvpx-1.14.1/lib/Mac/libvpx_fPIC.a',
)
ENGINE_NOTICES = (
    'Licenses/WebRTC_License.txt', 'Licenses/OpenSSL_License.txt',
    'Licenses/Opus_License.txt', 'Licenses/abseil-cpp_LICENSE.txt',
    'Licenses/nghttp2_License.txt', 'libOpus/opus-1.1/COPYING',
    'libOpus/opus-1.1/include/speex_resampler.h',
    'WebRTC/5414/Include/third_party/libyuv/LICENSE',
    'WebRTC/5414/Include/third_party/libsrtp/LICENSE',
    'WebRTC/5414/Include/third_party/libvpx/source/libvpx/LICENSE',
)


def file_record(path, expected=None, versions=False):
    artifacts.require(path.is_file(), f'Missing dependency input: {path}')
    result = {'sha256': artifacts.digest(path), 'bytes': path.stat().st_size}
    if expected is not None:
        artifacts.require(result['sha256'] == expected, f'Dependency checksum mismatch: {path}')
    if versions:
        # Embedded strings are evidence, not a reliable substitute for source provenance.
        strings = re.findall(rb'[\x20-\x7e]{5,}', path.read_bytes())
        result['version_strings'] = sorted({s.decode('ascii') for s in strings
            if re.fullmatch(rb'(?:libopus [\w.+-]+|OpenSSL 1\.1\.1\w+  +\d+ \w+ \d{4}|v1\.14\.1)', s)})
    return result


def check_libraries(engine, specs):
    return {artifacts.relative_path(s['path']): file_record(
        engine / artifacts.relative_path(s['path']), s['sha256'], versions=True) for s in specs}


def engine_distribution_provenance(engine, paths):
    """Confirm Epic distribution identities without inventing an upstream source revision."""
    manifest = engine / 'Engine/Build/Commit.gitdeps.xml'
    entries = {e.attrib['Name']: e.attrib['Hash'] for e in ET.parse(manifest).getroot().iter('File')}
    verified = {}
    for name in sorted(set(paths)):
        path = engine / artifacts.relative_path(name)
        artifacts.require(name in entries, 'No engine distribution identity for: ' + name)
        sha1 = hashlib.sha1(path.read_bytes()).hexdigest()
        artifacts.require(sha1 == entries[name], 'Engine distribution mismatch: ' + name)
        verified[name] = {'gitdeps_sha1': sha1, 'sha256': artifacts.digest(path)}
    return {'manifest_sha256': artifacts.digest(manifest), 'files': verified,
            'scope': 'Epic distribution bytes only; upstream source and security patches remain unverified'}


def audit_mobile(path, platform, engine, current_lock_hash):
    checked = artifacts.validate(path)
    artifacts.require(checked['platform'] == platform, f'Expected {platform} artifact.')
    manifest = artifacts.read_json(path / 'artifact.json')
    lock = artifacts.read_json(path / 'Build/build-lock.json')
    source = artifacts.read_json(path / 'Build/source-lock.json')
    lock_hash = artifacts.digest(path / 'Build/build-lock.json')
    artifacts.require(source['dependencies']['build-lock'] == lock_hash, 'Build-lock identity mismatch.')
    libraries = check_libraries(engine, lock['platforms'][platform]['link_libraries'])
    sections = re.findall(r'^# (.+)$', (path / 'Licenses/THIRD_PARTY_NOTICES.txt').read_text(), re.M)
    return {
        'validation': checked, 'metadata': manifest['metadata'],
        'build_lock_sha256': lock_hash, 'matches_current_build_lock': lock_hash == current_lock_hash,
        'source_dependencies': source['dependencies'],
        'patches': {p: manifest['files'][p] for p in source['patches']},
        'libraries': {name: file_record(path / name) for name in manifest['files'] if name.startswith('Lib/')},
        'engine_libraries': libraries,
        'notice_sections': sections,
        'notice_files': {name: digest for name, digest in manifest['files'].items() if name.startswith('Licenses/')},
        'link_probe': artifacts.read_json(path / 'Build/link-probe.json'),
    }


def audit(engine, paths, support):
    lock_path = ROOT / 'Config/MobileWebRTC.lock.json'
    lock = artifacts.read_json(lock_path)
    version = artifacts.read_json(engine / 'Engine/Build/Build.version')
    artifacts.require(tuple(version[k] for k in ('MajorVersion', 'MinorVersion', 'PatchVersion')) == (5, 8, 3),
                      'This review requires Unreal Engine 5.8.3.')
    headers = {}
    for name, spec in lock['engine_headers'].items():
        inventory = mobile_build.tree_inventory(engine / artifacts.relative_path(spec['path']))
        actual = mobile_build.tree_digest(inventory)
        artifacts.require(actual == spec['tree_sha256'], 'Engine header drift: ' + name)
        headers[name] = {'path': spec['path'], 'tree_sha256': actual, 'files': len(inventory)}
    mobile = {p: audit_mobile(path, p, engine, artifacts.digest(lock_path)) for p, path in paths.items()}
    support_check = ios_simulator_support.validate(support, require_current=False)
    support_lock = artifacts.read_json(support / 'source-lock.json')
    support_build = artifacts.read_json(support / 'build.json')
    rules = ('WebRTC/WebRTC.Build.cs', 'OpenSSL/OpenSSL.Build.cs',
             'libOpus/libOpus.Build.cs', 'libvpx/LibVpx.Build.cs')
    desktop_headers = mobile_build.tree_inventory(engine / (THIRD_PARTY + 'WebRTC/5414/Include'))
    engine_libraries = [THIRD_PARTY + p for p in MAC_LIBRARIES]
    engine_libraries.append(THIRD_PARTY + 'libOpus/opus-1.1/Android/ARM64/libspeex_resampler.a')
    for record in mobile.values():
        engine_libraries.extend(record['engine_libraries'])
    return {
        'schema': 2, 'scope': 'SR-003/SR-008 local inventory; see docs/DEPENDENCY_REVIEW.md',
        'security_approval': 'not_granted',
        'mobile_build_lock_sha256': artifacts.digest(lock_path),
        'engine': {
            'build_version': version, 'headers': headers,
            'distribution_provenance': engine_distribution_provenance(engine, engine_libraries),
            'webrtc_headers': {'tree_sha256': mobile_build.tree_digest(desktop_headers),
                               'files': len(desktop_headers)},
            'mac_libraries': {THIRD_PARTY + p: file_record(engine / (THIRD_PARTY + p), versions=True)
                              for p in MAC_LIBRARIES},
            'android_additional_library': file_record(engine / (THIRD_PARTY +
                'libOpus/opus-1.1/Android/ARM64/libspeex_resampler.a')),
            'rules': {THIRD_PARTY + p: artifacts.digest(engine / (THIRD_PARTY + p)) for p in rules},
            'notice_inputs': {THIRD_PARTY + p: artifacts.digest(engine / (THIRD_PARTY + p))
                              for p in ENGINE_NOTICES},
        },
        'mobile': mobile,
        'simulator_http2': {
            **support_check, 'version': support_lock['version'],
            'matches_current_source_lock': support_lock == artifacts.read_json(ios_simulator_support.LOCK),
            'source_sha256': support_lock['sha256'],
            'notice_sha256': artifacts.digest(support / 'LICENSE.txt'),
            'ndebug_in_recorded_flags': any(f.startswith('-DNDEBUG') for f in support_build['flags']),
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    for name in ('android', 'ios', 'simulator', 'simulator-support'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.engine, {'Android': args.android, 'IOS': args.ios, 'IOSSimulator': args.simulator},
                   args.simulator_support)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print('Inventory verified. Security/release approval is not granted. Report: ' + str(args.output))


if __name__ == '__main__':
    main()
