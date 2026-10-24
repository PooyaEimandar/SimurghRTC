#!/usr/bin/env python3
"""Inventory compiled sources from an accepted native build, without changing it."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess

import mobile_artifacts as a
import native_artifacts


def run(*command, cwd=None):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def archive_objects(text):
    # These builders use GN's complete_static_lib: every explicit archive input
    # is an object. Refuse other graphs rather than overlooking nested libraries.
    lines = [line for line in text.replace('$\n', '').splitlines()
             if line.startswith('build obj/simurgh_probe/libprobe.a: ')]
    a.require(len(lines) == 1, 'Expected one complete native archive rule.')
    objects = lines[0].split(' |', 1)[0].split()[3:]
    a.require(objects and len(set(objects)) == len(objects)
              and all(name.startswith('obj/') and name.endswith('.o') for name in objects),
              'Archive must contain distinct target objects only.')
    return objects


def compile_sources(build, objects):
    wanted = set(objects)
    found = {}
    for ninja in sorted((build / 'obj').rglob('*.ninja')):
        for line in ninja.read_text().replace('$\n', '').splitlines():
            if not line.startswith('build '):
                continue
            outputs, separator, inputs = line[6:].partition(': ')
            names = outputs.split()
            if not separator or not names or names[0] not in wanted:
                continue
            obj = names[0]
            fields = inputs.split(' |', 1)[0].split()
            a.require(len(fields) == 2 and fields[0] in ('cc', 'cxx', 'objc', 'objcxx', 'asm')
                      and names[1:] in ([], [obj[:-2] + '.dwo']),
                      'Unsupported object rule: ' + line)
            a.require(obj not in found, 'Duplicate object rule: ' + obj)
            found[obj] = (build / fields[1]).resolve()
    a.require(set(found) == wanted, 'Missing compile rules for native archive objects.')
    return found


def component(path):
    parts = path.split('/')
    if 'third_party' in parts:
        i = parts.index('third_party')
        return '/'.join(parts[:i + 2])
    return 'simurgh_bridge' if parts[0] == 'simurgh_probe' else 'webrtc'


def audit(work, artifact):
    identity = native_artifacts.validate(artifact)
    record = a.read_json(artifact / 'Build/build.json')
    build = Path(record['build_directory']).resolve()
    source = work / 'src'
    a.require(build.is_relative_to(source / 'out'), 'Build does not belong to source workspace.')
    a.require(a.digest(build / 'args.gn') == a.digest(artifact / 'Build/args.gn'), 'Build flags changed.')
    archive = build / 'obj/simurgh_probe/libprobe.a'
    a.require(a.digest(archive) == record['archive_before_isolation_sha256'], 'Compiled archive changed.')
    lock = a.read_json(artifact / 'Build/source-lock.json')
    patches = lock['patches'] + lock.get('backend_patches', [])
    allowed = {}
    for patch in patches:
        for name, hashes in patch['files'].items():
            allowed.setdefault(patch['repository'], {})[name] = hashes['after']
    repos = {}
    tracked = {}
    for name, spec in lock['repositories'].items():
        path = work / name
        a.require(run('git', 'rev-parse', 'HEAD', cwd=path) == spec['revision'], 'Source revision drift: ' + name)
        a.require(run('git', 'remote', 'get-url', 'origin', cwd=path) == spec['url'], 'Source remote drift: ' + name)
        changed = set(run('git', 'diff', '--name-only', 'HEAD', cwd=path).splitlines())
        a.require(changed == set(allowed.get(name, {})), 'Unexpected source edits: ' + name)
        for filename, digest in allowed.get(name, {}).items():
            a.require(a.digest(path / filename) == digest, 'Patch content drift: ' + filename)
        repos[name] = dict(spec, modified_files=sorted(changed))
        tracked[name] = set(run('git', 'ls-files', '-z', cwd=path).split('\0'))
    for name, digest in record['bridge_sources'].items():
        if name in ('BUILD.gn', 'runtime.cc', 'media.cc', 'simurgh_rtc_abi.h'):
            a.require(a.digest(source / 'simurgh_probe' / name) == digest, 'Bridge source drift: ' + name)
    objects = archive_objects((build / 'obj/simurgh_probe/probe.ninja').read_text())
    sources = compile_sources(build, objects)
    compiled = []
    for obj, path in sorted(sources.items()):
        a.require(path.is_relative_to(source), 'Source escaped workspace: ' + str(path))
        generated = path.is_relative_to(build / 'gen')
        relative = path.relative_to(build / 'gen' if generated else source).as_posix()
        repo = None if generated or relative.startswith('simurgh_probe/') else max(
            (name for name in repos if path.is_relative_to(work / name)), key=len)
        if repo:
            a.require(path.relative_to(work / repo).as_posix() in tracked[repo],
                      'Untracked compiled source: ' + relative)
        compiled.append({'object': obj, 'source': relative, 'generated': generated,
                         'component': component(relative), 'repository': repo, 'sha256': a.digest(path)})
    # A canonical source list is useful across build roots; debug paths do not
    # change which input files the final archive actually contains.
    inputs = sorted({(row['source'], row['generated'], row['sha256']) for row in compiled})
    components = dict(sorted(Counter(row['component'] for row in compiled).items()))
    return {'schema': 1, 'passed': True, 'platform': identity['platform'],
            'artifact_manifest_sha256': identity['manifest_sha256'],
            'archive_sha256': a.digest(artifact / 'Lib/libwebrtc.a'),
            'archive_before_isolation_sha256': record['archive_before_isolation_sha256'],
            'build_directory': str(build), 'objects': len(compiled), 'components': components,
            'additional_link_inputs': ({'ndk_compiler_runtime_sha256': record['compiler_runtime_sha256']}
                                       if identity['platform'] == 'Android' else {}),
            'source_inventory_sha256': hashlib.sha256(json.dumps(inputs, separators=(',', ':')).encode()).hexdigest(),
            'repositories': repos, 'compiled_sources': compiled,
            'scope': 'compiled archive inputs and pinned source trees; not an advisory clearance or final app inventory'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('work', 'artifact', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    a.require(not args.output.exists(), 'Use a new report path.')
    result = audit(args.work.resolve(), args.artifact.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('passed', 'platform', 'objects', 'components')}))


if __name__ == '__main__':
    main()
