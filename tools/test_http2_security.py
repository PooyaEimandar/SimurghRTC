#!/usr/bin/env python3
"""Run the HTTP/2 regression against vulnerable/current sources and Unreal headers on Mac."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import resource
import signal
import subprocess
import tarfile

import mobile_artifacts as artifacts
import mobile_build
import ios_simulator_support

PROBE = Path(__file__).resolve().parents[1] / 'Config/IOSSimulatorProbe/http2_regression.c'


def test(engine, baseline_archive, current_archive, output):
    artifacts.require(not output.exists(), 'Use a new HTTP/2 regression output directory.')
    lock = artifacts.read_json(ios_simulator_support.LOCK)
    headers = engine / 'Engine/Source/ThirdParty/nghttp2' / lock['engine_header_version'] / 'include'
    for name, digest in lock['engine_headers'].items():
        artifacts.require(artifacts.digest(headers / 'nghttp2' / name) == digest, 'Engine header drift: ' + name)
    output.mkdir(parents=True)
    report = {'cve': lock['security_fix']['cve'], 'probe_sha256': artifacts.digest(PROBE),
              'compiler': mobile_build.run(['xcrun', 'clang', '--version']),
              'engine_headers': lock['engine_headers'], 'runs': {}}
    for label, archive, spec in [('baseline', baseline_archive, lock['regression_baseline']),
                                 ('current', current_archive, lock)]:
        artifacts.require(artifacts.digest(archive) == spec['sha256'], 'HTTP/2 source archive checksum mismatch.')
        work = output / label
        source = work / 'source'
        source.mkdir(parents=True)
        # Both tarballs are pinned before extraction; only ordinary C-library inputs are used.
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                parts = Path(member.name).parts
                artifacts.require(parts and parts[0] == 'nghttp2-' + spec['version'] and '..' not in parts,
                                  'Unsafe HTTP/2 source path.')
                if len(parts) < 2 or parts[1] != 'lib':
                    continue
                artifacts.require(member.isdir() or member.isfile(), 'Unsupported HTTP/2 source entry.')
                path = source.joinpath(*parts[1:])
                if member.isdir():
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with tar.extractfile(member) as incoming:
                        path.write_bytes(incoming.read())

        def compile_source(path):
            obj = work / (path.stem + '.o')
            mobile_build.run(['xcrun', 'clang', '-O2', '-g', '-DNGHTTP2_STATICLIB', '-DBUILDING_NGHTTP2',
                              '-DHAVE_ARPA_INET_H=1', '-I' + str(source / 'lib/includes'), '-c', path, '-o', obj])
            return obj

        with ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_source, sorted((source / 'lib').glob('*.c'))))
        library, executable = work / 'libnghttp2.a', work / 'regression'
        mobile_build.run(['xcrun', 'ar', 'rcs', library, *objects])
        # Deliberately use the old Unreal headers to check the exercised public ABI.
        mobile_build.run(['xcrun', 'clang', '-O0', '-g', '-DNGHTTP2_STATICLIB',
                          '-I' + str(headers), PROBE, library, '-o', executable])
        runs = []
        for mode in range(3):
            def no_core():
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            result = subprocess.run([str(executable), str(mode)], capture_output=True, text=True,
                                    timeout=15, preexec_fn=no_core)
            runs.append({'mode': mode, 'exit_code': result.returncode,
                         'stdout': result.stdout.strip(), 'stderr': result.stderr.strip()})
        report['runs'][label] = {'source_sha256': spec['sha256'], 'library_sha256': artifacts.digest(library),
                                 'cases': runs}
    baseline, current = (report['runs'][label]['cases'] for label in ('baseline', 'current'))
    ok = (baseline[0]['exit_code'] == 0
          and all(r['exit_code'] == -signal.SIGABRT and 'iframe->state == NGHTTP2_IB_IGN_ALL' in r['stderr']
                  for r in baseline[1:])
          and all(r['exit_code'] == 0 and f'runtime={lock["version"]} headers={lock["engine_header_version"]}'
                  in r['stdout'] for r in current))
    report['status'] = 'passed' if ok else 'failed'
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    artifacts.require(ok, 'HTTP/2 regression failed; see ' + str(output / 'result.json'))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('engine', 'baseline-archive', 'current-archive', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    report = test(args.engine.resolve(), args.baseline_archive.resolve(), args.current_archive.resolve(),
                  args.output.resolve())
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
