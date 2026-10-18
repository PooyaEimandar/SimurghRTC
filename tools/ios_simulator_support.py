#!/usr/bin/env python3
"""Build the missing simulator HTTP/2 dependency for the UE 5.8.3 probe only."""
from __future__ import annotations
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
import tarfile
import tempfile

import mobile_artifacts as artifacts
import mobile_build

LOCK = Path(__file__).resolve().parents[1] / 'Config/IOSSimulatorProbe.lock.json'


def validate(root):
    files = artifacts.payload_files(root)
    record = artifacts.read_json(root / 'support.json')
    artifacts.require(record.get('schema') == 1 and record.get('platform') == 'IOSSimulator'
                      and record.get('architecture') == 'arm64', 'Invalid simulator support metadata.')
    required = {'libnghttp2.a', 'LICENSE.txt', 'build.json', 'source-lock.json'}
    artifacts.require(set(record['files']) == required and set(files) == required | {'support.json'},
                      'Unexpected simulator support payload.')
    for name in required:
        artifacts.require(artifacts.digest(files[name]) == record['files'][name], 'Simulator support file changed: ' + name)
    artifacts.require(record['archive_sha256'] == record['files']['libnghttp2.a'], 'Support archive identity mismatch.')
    artifacts.validate_archive(root / 'libnghttp2.a', 'IOSSimulator')
    return {'manifest_sha256': artifacts.digest(root / 'support.json'), 'archive_sha256': record['archive_sha256']}


def build(engine, output, archive=None):
    artifacts.require(not output.exists(), 'Use a new output directory for simulator support.')
    lock = artifacts.read_json(LOCK)
    mobile_lock = mobile_build.load_lock()
    toolchain = mobile_build.preflight(mobile_lock, 'IOSSimulator', Path(sys.executable))
    engine_headers = engine / 'Engine/Source/ThirdParty/nghttp2' / lock['version'] / 'include/nghttp2'
    for name, digest in lock['engine_headers'].items():
        artifacts.require(artifacts.digest(engine_headers / name) == digest, 'Unreal nghttp2 header drift: ' + name)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='simulator-http2-', dir=output.parent) as temporary:
        work = Path(temporary)
        if archive is None:
            archive = work / 'source.tar.gz'
            mobile_build.download(lock, archive)
        artifacts.require(artifacts.digest(archive) == lock['sha256'], 'nghttp2 source checksum mismatch.')
        source = work / 'source'
        source.mkdir()
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                parts = PurePosixPath(member.name).parts
                artifacts.require(parts and parts[0] == 'nghttp2-' + lock['version'] and '..' not in parts,
                                  'Unsafe nghttp2 archive path.')
                if len(parts) < 2 or parts[1] not in ('lib', 'COPYING'):
                    continue
                artifacts.require(member.isdir() or member.isfile(), 'Unsupported nghttp2 archive entry.')
                path = source.joinpath(*parts[1:])
                if member.isdir():
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with tar.extractfile(member) as incoming, path.open('wb') as outgoing:
                        shutil.copyfileobj(incoming, outgoing)
        for name, digest in lock['engine_headers'].items():
            artifacts.require(artifacts.digest(source / 'lib/includes/nghttp2' / name) == digest,
                              'Upstream and Unreal nghttp2 headers differ: ' + name)
        sdk = mobile_build.run(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'])
        flags = ['-target', 'arm64-apple-ios15.0-simulator', '-isysroot', sdk, '-O2', '-fPIC',
                 '-fvisibility=hidden', '-DNGHTTP2_STATICLIB', '-DBUILDING_NGHTTP2', '-DHAVE_ARPA_INET_H=1',
                 '-I' + str(source / 'lib/includes')]
        objects = []
        for path in sorted((source / 'lib').glob('*.c')):
            obj = work / (path.stem + '.o')
            mobile_build.run(['xcrun', 'clang', *flags, '-c', path, '-o', obj], log=work / 'compile.log')
            objects.append(obj)
        artifacts.require(objects, 'nghttp2 source archive contains no library sources.')
        output.mkdir()
        mobile_build.run(['xcrun', 'ar', 'rcs', output / 'libnghttp2.a', *objects])
        mobile_build.run(['xcrun', 'strip', '-S', output / 'libnghttp2.a'])
        artifacts.validate_archive(output / 'libnghttp2.a', 'IOSSimulator')
        shutil.copy2(source / 'COPYING', output / 'LICENSE.txt')
        shutil.copy2(LOCK, output / 'source-lock.json')
        (output / 'build.json').write_text(json.dumps({'toolchain': toolchain, 'flags': flags,
            'source_files': [p.name for p in objects], 'recipe_sha256': artifacts.digest(Path(__file__))}, indent=2) + '\n')
        files = {name: artifacts.digest(output / name) for name in ['libnghttp2.a', 'LICENSE.txt', 'build.json', 'source-lock.json']}
        (output / 'support.json').write_text(json.dumps({'schema': 1, 'platform': 'IOSSimulator',
            'architecture': 'arm64', 'archive_sha256': files['libnghttp2.a'], 'files': files}, indent=2) + '\n')
    return validate(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archive', type=Path, help='Optional local copy of the pinned source download.')
    args = parser.parse_args()
    print(json.dumps(build(args.engine.resolve(), args.output.resolve(), args.archive), indent=2))


if __name__ == '__main__':
    main()
