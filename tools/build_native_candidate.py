#!/usr/bin/env python3
"""Build a Mac-only WebRTC replacement proof; never installs it into the Unreal plugin."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

import mobile_artifacts as artifacts
import mobile_build

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'Config/NativeWebRTC'
LOCK = ROOT / 'Config/NativeWebRTC.candidate.lock.json'
EXPORTS = {'_SimurghRTC_NativeProbeV1', '_SimurghRTC_NativeVersionV1'}


def source_inputs(work, lock):
    """Fetch exact revisions without resetting or adopting another checkout."""
    for name, spec in sorted(lock['repositories'].items(), key=lambda item: (item[0].count('/'), item[0])):
        artifacts.require(len(spec['revision']) == 40
                          and all(c in '0123456789abcdef' for c in spec['revision']), 'Unpinned source: ' + name)
        artifacts.require(spec['url'].startswith('https://'), 'Source URLs must use HTTPS.')
        path = mobile_build.owned_path(work, name)
        if not (path / '.git').exists():
            artifacts.require(not path.exists() or not any(path.iterdir()), 'Nonempty source directory: ' + name)
            path.mkdir(parents=True, exist_ok=True)
            mobile_build.run(['git', 'init', '-q', path])
            mobile_build.run(['git', 'remote', 'add', 'origin', spec['url']], cwd=path)
        artifacts.require(mobile_build.run(['git', 'remote', 'get-url', 'origin'], cwd=path) == spec['url'],
                          'Source remote drift: ' + name)
        head = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=path, capture_output=True, text=True)
        if head.returncode:
            print('Fetching ' + name, flush=True)
            mobile_build.run(['git', 'fetch', '--depth=1', 'origin', spec['revision']], cwd=path)
            mobile_build.run(['git', 'checkout', '--detach', 'FETCH_HEAD'], cwd=path)
        artifacts.require(mobile_build.run(['git', 'rev-parse', 'HEAD'], cwd=path) == spec['revision'],
                          'Source revision drift: ' + name)
        changed = set(mobile_build.run(['git', 'diff', '--name-only', 'HEAD'], cwd=path).splitlines())
        artifacts.require(changed <= ({'BUILD.gn'} if name == 'src' else set()), 'Edited source: ' + name)
        generated = {'src': {'out/', 'simurgh_probe/'},
                     'src/build': {'config/gclient_args.gni'},
                     'src/third_party': {'llvm-build/'}}.get(name, set())
        nested = {child[len(name) + 1:] + '/' for child in lock['repositories'] if child.startswith(name + '/')}
        untracked = set(filter(None, mobile_build.run(
            ['git', 'ls-files', '--others', '--directory', '-z'], cwd=path).split('\0')))
        artifacts.require(untracked <= generated | nested,
                          'Unexpected source files in ' + name + ': ' + str(sorted(untracked - generated - nested)))
    patch = lock['patches'][0]
    artifacts.require(artifacts.digest(ROOT / patch['path']) == patch['sha256'], 'Candidate patch drift.')
    path = work / 'src/BUILD.gn'
    hashes = patch['files']['BUILD.gn']
    actual = artifacts.digest(path)
    artifacts.require(actual in hashes.values(), 'Candidate root build file changed.')
    if actual == hashes['before']:
        mobile_build.run(['git', 'apply', '--check', ROOT / patch['path']], cwd=work / 'src')
        mobile_build.run(['git', 'apply', ROOT / patch['path']], cwd=work / 'src')
    artifacts.require(artifacts.digest(path) == hashes['after'], 'Candidate patch result mismatch.')
    artifacts.require(artifacts.digest(work / 'src/DEPS') == lock['upstream_deps_sha256'], 'DEPS drift.')


def build_tools(work, lock):
    host = work / 'host'
    host.mkdir(exist_ok=True)
    for name in ('gn', 'ninja'):
        archive = host / (name + '.zip')
        mobile_build.download(lock['tools'][name], archive)
        with zipfile.ZipFile(archive) as source:
            (host / name).write_bytes(source.read(name))
        (host / name).chmod(0o755)
    archive = work / 'downloads/clang.tar.xz'
    mobile_build.download(lock['tools']['clang'], archive)
    compiler = work / 'src/third_party/llvm-build/Release+Asserts'
    compiler.mkdir(parents=True, exist_ok=True)
    # Python's data filter rejects path escapes and unsafe links. Only pinned bytes are extracted.
    with tarfile.open(archive) as source:
        source.extractall(compiler, filter='data')
    return host


def run(work, engine, output, jobs, build_name='MacCandidate'):
    artifacts.require(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                      'This candidate proof currently supports Mac ARM64 only.')
    artifacts.require(sys.version_info >= (3, 12), 'Use Python 3.12 or newer for filtered tar extraction.')
    artifacts.require(re.fullmatch(r'[A-Za-z0-9_-]+', build_name), 'Build name must be a single directory name.')
    artifacts.require(not output.exists(), 'Use a new candidate report directory.')
    lock = artifacts.read_json(LOCK)
    artifacts.require(lock['schema'] == 1 and lock['status'] == 'candidate_only', 'Unsupported candidate lock.')
    artifacts.require(mobile_build.run(['xcodebuild', '-version']) == lock['mac']['xcode'], 'Candidate Xcode drift.')
    artifacts.require(mobile_build.run(['xcrun', '--sdk', 'macosx', '--show-sdk-version']) == lock['mac']['sdk'],
                      'Candidate macOS SDK drift.')
    for name, digest in lock['coexistence_engine_libraries'].items():
        artifacts.require(artifacts.digest(engine / name) == digest, 'Unreal comparison library changed: ' + name)
    with mobile_build.workspace(work) as work:
        mobile_build.free_space(work, 12)
        source_inputs(work, lock)
        host = build_tools(work, lock)
        source = work / 'src'
        probe = source / 'simurgh_probe'
        probe.mkdir(exist_ok=True)
        for name in ('BUILD.gn', 'probe.cc'):
            shutil.copy2(CONFIG / name, probe / name)
        (source / 'build/config/gclient_args.gni').write_text(
            'android_ndk_version = "2@30.0.16248370"\ngenerate_location_tags = true\n')
        build = mobile_build.owned_path(work, 'src/out/' + build_name)
        build.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CONFIG / 'mac-probe.gn', build / 'args.gn')
        output.mkdir(parents=True)
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', ZERO_AR_DATE='1',
                   SOURCE_DATE_EPOCH=str(lock['source_epoch']), LC_ALL='C', TZ='UTC')
        for name in ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH', 'C_INCLUDE_PATH',
                     'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'SDKROOT'):
            env.pop(name, None)
        mobile_build.run([host / 'gn', 'gen', build, '--root=' + str(source)], env=env, log=output / 'gn.log')
        mobile_build.run([host / 'ninja', '-C', build, '-j', jobs, 'simurgh_probe:probe'], env=env, log=output / 'build.log')
        archive = build / 'obj/simurgh_probe/libprobe.a'
        isolated = output / 'isolated.o'
        mobile_build.run(['xcrun', 'ld', '-r', '-arch', 'arm64',
                          '-platform_version', 'macos', lock['mac']['minimum_os'], lock['mac']['sdk'],
                          '-oso_prefix', str(build) + '/',
                          '-exported_symbols_list', CONFIG / 'exports.txt',
                          '-all_load', archive, '-o', isolated], env=env, log=output / 'isolation.log')
        symbols = mobile_build.run(['xcrun', 'nm', '-g', '-U', isolated])
        exports = {line.split()[-1] for line in symbols.splitlines() if line.split()}
        artifacts.require(exports == EXPORTS, 'Candidate exposes unexpected native symbols: ' + str(exports))
        third_party = engine / 'Engine/Source/ThirdParty'
        include = ['OpenSSL/1.1.1t/include/Mac', 'libOpus/opus-1.1/include', 'libvpx/libvpx-1.14.1/include']
        client = output / 'coexistence.o'
        mobile_build.run(['xcrun', 'clang', '-c', CONFIG / 'coexistence.c', '-o', client,
                          *['-I' + str(third_party / name) for name in include]], env=env, log=output / 'link.log')
        executable = output / 'coexistence'
        mobile_build.run(['xcrun', 'clang++', client, isolated,
                          *[engine / name for name in lock['coexistence_engine_libraries']],
                          '-framework', 'Foundation', '-framework', 'Security', '-framework', 'SystemConfiguration',
                          '-framework', 'CoreAudio', '-framework', 'AudioToolbox', '-o', executable], env=env, log=output / 'link.log')
        result = subprocess.run([str(executable)], env=env, capture_output=True, text=True, timeout=120)
        (output / 'runtime.log').write_text(result.stdout + result.stderr)
        artifacts.require(result.returncode == 0, 'Candidate coexistence/lifecycle probe failed; see runtime.log.')
        report = {'schema': 1, 'status': 'passed', 'scope': 'Mac ARM64 standalone dependency coexistence only',
                  'security_approval': 'not_granted', 'unreal_package_tested': False, 'mobile_tested': False,
                  'source_revision': lock['repositories']['src']['revision'], 'lock_sha256': artifacts.digest(LOCK),
                  'build_name': build_name,
                  'archive_sha256': artifacts.digest(archive), 'isolated_sha256': artifacts.digest(isolated),
                  'exports': sorted(exports), 'runtime_stdout': result.stdout,
                  'compiler': mobile_build.run([source / 'third_party/llvm-build/Release+Asserts/bin/clang', '--version']),
                  'python': sys.version.split()[0], 'recipe_sha256': artifacts.digest(Path(__file__)),
                  'probe_sources': {p.name: artifacts.digest(p) for p in CONFIG.iterdir() if p.is_file()}}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('work', 'engine', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--build-name', default='MacCandidate', help='Use a fresh name for a separate Ninja output directory.')
    args = parser.parse_args()
    artifacts.require(1 <= args.jobs <= 16, 'Use 1–16 build jobs.')
    print(json.dumps(run(args.work.resolve(), args.engine.resolve(), args.output.resolve(), args.jobs, args.build_name), indent=2))


if __name__ == '__main__':
    main()
