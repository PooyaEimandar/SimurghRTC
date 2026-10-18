#!/usr/bin/env python3
"""Package and check separately built ARM64 WebRTC inputs for the experimental M0 adapter."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct

MANIFEST = 'artifact.json'
ADAPTER = 'webrtc-5414-ue5.8.3-v1'
REQUIRED = (
    'Lib/libwebrtc.a', 'Include/api/create_peerconnection_factory.h',
    'Include/rtc_base/thread.h', 'Include/third_party/abseil-cpp/absl/base/config.h',
    'Include/third_party/libyuv/include/libyuv.h',
    'Build/args.gn', 'Build/source-lock.json', 'Build/recipe.txt',
    'Licenses/THIRD_PARTY_NOTICES.txt',
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def read_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)


def relative_path(value: str) -> str:
    require(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_./+-]+', value) is not None,
            'Artifact paths must be portable relative paths.')
    path = PurePosixPath(value)
    require(bool(path.parts) and not path.is_absolute() and str(path) == value
            and all(part not in ('.', '..') for part in path.parts), f'Unsafe path: {value}')
    return value


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def payload_files(root: Path) -> dict[str, Path]:
    require(root.is_dir() and not root.is_symlink(), 'Artifact must be a real directory.')
    files = {}
    folded = set()
    for path in sorted(root.rglob('*')):
        require(not path.is_symlink(), 'Artifact symlinks are not supported.')
        name = relative_path(path.relative_to(root).as_posix())
        require(name.casefold() not in folded, f'Case-colliding artifact path: {name}')
        folded.add(name.casefold())
        if path.is_dir():
            continue
        require(path.is_file(), f'Artifact contains a non-regular file: {name}')
        if name != MANIFEST:
            files[name] = path
    return files


def validate_metadata(meta: dict) -> None:
    keys = {'schema', 'artifact_id', 'platform', 'architecture', 'engine_version', 'adapter',
            'configuration', 'dependencies', 'source_revision', 'toolchain', 'codecs'}
    require(isinstance(meta, dict) and set(meta) == keys, 'Unexpected or missing artifact metadata fields.')
    require(type(meta['schema']) is int and meta['schema'] == 1, 'Unsupported artifact schema.')
    require(isinstance(meta['artifact_id'], str) and re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,95}', meta['artifact_id']),
            'Invalid artifact_id.')
    require(meta['platform'] in ('Android', 'IOS'), 'Only Android and IOS device artifacts are supported.')
    require(meta['architecture'] == 'arm64', 'Only ARM64 artifacts are supported.')
    require(meta['engine_version'] == '5.8.3' and meta['adapter'] == ADAPTER,
            'Artifact requires a different engine or private adapter.')
    require(meta['configuration'] == 'Release', 'Only Release dependencies are supported.')
    require(meta['dependencies'] == 'unreal-openssl-opus-vpx',
            'This adapter requires Unreal OpenSSL, Opus and VPX; embedded copies are incompatible.')
    require(isinstance(meta['source_revision'], str) and re.fullmatch(r'[0-9a-f]{40}', meta['source_revision'])
            and len(set(meta['source_revision'])) > 1, 'Record the exact WebRTC Git commit.')
    toolchain = meta['toolchain']
    require(isinstance(toolchain, dict) and set(toolchain) == {'host', 'compiler', 'sdk', 'minimum_os', 'cpp_runtime'},
            'Record host, compiler, SDK, minimum OS and C++ runtime.')
    for value in toolchain.values():
        require(isinstance(value, str) and value.strip() == value and 0 < len(value) <= 256
                and not any(ord(c) < 32 for c in value), 'Toolchain values must be nonempty single-line strings.')
    require(toolchain['host'] == ('Linux' if meta['platform'] == 'Android' else 'Mac'), 'Wrong dependency build host.')
    require(toolchain['cpp_runtime'] == 'libc++', 'The adapter requires the platform libc++.')
    codecs = meta['codecs']
    require(isinstance(codecs, dict) and set(codecs) == {'audio', 'video'}, 'Record audio and video codecs.')
    for kind, required in [('audio', 'opus'), ('video', 'VP8')]:
        values = codecs[kind]
        require(isinstance(values, list) and all(isinstance(v, str) and re.fullmatch(r'[A-Za-z0-9.-]+', v) for v in values)
                and len(set(values)) == len(values) and required in values,
                f'The initial media probe requires {required}; record unique codec names.')


def validate_object(data: bytes, platform: str) -> None:
    if platform == 'Android':
        require(len(data) >= 64 and data[:6] == b'\x7fELF\x02\x01', 'Expected a little-endian ELF64 object.')
        kind, machine = struct.unpack_from('<HH', data, 16)
        require(kind == 1 and machine == 183, 'Android archives must contain only relocatable AArch64 objects.')
        return
    require(len(data) >= 32 and data[:4] == b'\xcf\xfa\xed\xfe', 'Expected a thin 64-bit Mach-O object.')
    cpu, subtype, kind, commands, command_bytes = struct.unpack_from('<IIIII', data, 4)
    require(cpu == 0x100000C and subtype == 0 and kind == 1, 'iOS archives must contain only device arm64 objects.')
    require(32 + command_bytes <= len(data), 'Truncated Mach-O load commands.')
    offset = 32
    device = False
    for _ in range(commands):
        require(offset + 8 <= 32 + command_bytes, 'Truncated Mach-O load command.')
        command, size = struct.unpack_from('<II', data, offset)
        require(size >= 8 and offset + size <= 32 + command_bytes, 'Invalid Mach-O load command size.')
        if command == 0x32:  # LC_BUILD_VERSION: iOS=2, iOS simulator=7.
            require(size >= 24 and struct.unpack_from('<I', data, offset + 8)[0] == 2,
                    'iOS simulator or non-iOS object found in device artifact.')
            device = True
        elif command == 0x25:  # LC_VERSION_MIN_IPHONEOS, older device builds.
            require(size >= 16, 'Truncated iOS deployment command.')
            device = True
        elif command in (0x24, 0x2F, 0x30):
            raise ValueError('Non-iOS deployment command in device artifact.')
        offset += size
    require(offset == 32 + command_bytes and device, 'Missing iOS device platform metadata.')


def validate_archive(path: Path, platform: str) -> int:
    """Read one member at a time; reject thin/fat archives, bitcode and mixed architectures."""
    count = 0
    with path.open('rb') as stream:
        require(stream.read(8) == b'!<arch>\n', 'Expected a normal static ar archive (no thin archives).')
        while header := stream.read(60):
            require(len(header) == 60 and header[58:] == b'`\n', 'Malformed archive member header.')
            try:
                name = header[:16].decode('ascii').rstrip()
                size = int(header[48:58].decode('ascii').strip())
            except (ValueError, UnicodeDecodeError) as error:
                raise ValueError('Malformed archive member name or size.') from error
            require(0 <= size <= 256 * 1024 * 1024, 'Invalid or excessive archive member size.')
            data = stream.read(size)
            require(len(data) == size, 'Truncated archive member.')
            if size % 2:
                require(stream.read(1) == b'\n', 'Invalid archive padding.')
            if name.startswith('#1/'):
                require(name[3:].isdigit(), 'Invalid BSD archive name.')
                length = int(name[3:])
                require(0 < length <= len(data), 'Truncated BSD archive name.')
                name = data[:length].rstrip(b'\0').decode('utf-8')
                data = data[length:]
            if name in ('/', '//', '/SYM64/') or name.rstrip('/') in ('__.SYMDEF', '__.SYMDEF SORTED', '__.SYMDEF_64', '__.SYMDEF_64 SORTED'):
                continue
            validate_object(data, platform)
            count += 1
    require(count > 0, 'Archive contains no native objects.')
    return count


def validate(root: Path) -> dict:
    document = read_json(root / MANIFEST)
    require(isinstance(document, dict) and set(document) == {'metadata', 'files'}, 'Expected metadata and files.')
    meta, inventory = document['metadata'], document['files']
    validate_metadata(meta)
    require(isinstance(inventory, dict), 'Expected a file-to-SHA256 inventory.')
    files = payload_files(root)
    require(set(files) == set(inventory), 'Artifact file inventory differs from disk.')
    require(set(REQUIRED) <= set(files), 'Artifact is missing headers, archive, build records or notices.')
    for name, path in files.items():
        expected = inventory[name]
        require(isinstance(expected, str) and re.fullmatch(r'[0-9a-f]{64}', expected), f'Invalid SHA256: {name}')
        require(digest(path) == expected, f'SHA256 mismatch: {name}')
        if path.suffix.lower() in ('.a', '.so', '.dylib', '.dll', '.lib', '.jar', '.aar'):
            require(name == 'Lib/libwebrtc.a', 'v1 accepts exactly one native archive and no Java/helper binaries.')
    for name in REQUIRED:
        require(files[name].stat().st_size > 0, f'Empty required file: {name}')
    lock = read_json(files['Build/source-lock.json'])
    require(isinstance(lock, dict) and lock.get('webrtc_revision') == meta['source_revision']
            and isinstance(lock.get('dependencies'), dict) and bool(lock['dependencies'])
            and isinstance(lock.get('patches'), list), 'Source lock must record matching WebRTC, dependencies and patches.')
    for name, revision in lock['dependencies'].items():
        require(isinstance(name, str) and name and isinstance(revision, str)
                and re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', revision), 'Pin dependency revisions or checksums in the source lock.')
    for patch in lock['patches']:
        require(isinstance(patch, str) and relative_path(patch).startswith('Build/patches/')
                and patch in files, 'Source-lock patches must be included and checksummed.')
    objects = validate_archive(files['Lib/libwebrtc.a'], meta['platform'])
    return {'artifact_id': meta['artifact_id'], 'platform': meta['platform'], 'architecture': meta['architecture'],
            'files': len(files), 'objects': objects, 'archive_bytes': files['Lib/libwebrtc.a'].stat().st_size,
            'manifest_sha256': digest(root / MANIFEST), 'runtime_validation': 'unverified'}


def package(source: Path, metadata: Path, output: Path) -> dict:
    source, output = source.resolve(), output.absolute()
    meta = read_json(metadata)
    validate_metadata(meta)
    require(not output.exists() and not output.is_symlink(), 'Output must not exist.')
    require(output.resolve() != source and source not in output.resolve().parents, 'Output must be outside the input tree.')
    files = payload_files(source)
    require(not (source / MANIFEST).exists(), 'Input already contains an artifact manifest; use check instead.')
    inventory = {name: digest(path) for name, path in files.items()}
    # A failed package remains available for inspection; never remove existing user files.
    shutil.copytree(source, output)
    (output / MANIFEST).write_text(json.dumps({'metadata': meta, 'files': inventory}, indent=2, sort_keys=True) + '\n')
    return validate(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    check = commands.add_parser('check', help='Verify hashes, records and native object architecture/platform.')
    check.add_argument('artifact', type=Path)
    pack = commands.add_parser('package', help='Inventory an existing build; this does not compile WebRTC.')
    pack.add_argument('--input', type=Path, required=True)
    pack.add_argument('--metadata', type=Path, required=True)
    pack.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(args.artifact) if args.command == 'check' else package(args.input, args.metadata, args.output)
    except (ValueError, OSError, UnicodeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
