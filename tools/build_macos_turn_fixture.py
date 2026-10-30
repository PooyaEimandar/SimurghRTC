#!/usr/bin/env python3
"""Build the pinned coturn fixture locally; record the installed Mac build dependencies."""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request
import mobile_artifacts as a

ROOT = Path(__file__).resolve().parents[1]


def build(output):
    a.require(not output.exists(), 'Use a fresh fixture directory.')
    output.mkdir(parents=True)
    lock = a.read_json(ROOT/'Fixtures/services/sources.lock.json')['coturn']
    archive = output/'source.tar.gz'
    with urllib.request.urlopen(lock['archive_url'], timeout=60) as response:
        archive.write_bytes(response.read())
    a.require(a.digest(archive) == lock['archive_sha256'], 'coturn source digest mismatch.')
    extracted = output/'extracted'; extracted.mkdir()
    with tarfile.open(archive) as tar: tar.extractall(extracted, filter='data')
    roots = list(extracted.iterdir()); a.require(len(roots)==1 and roots[0].is_dir(), 'Unexpected source root.')
    source = roots[0]
    dependencies = {}
    for name in ('libevent','openssl@3'):
        prefix = Path('/opt/homebrew/opt')/name
        dependencies[name] = {'receipt': a.read_json(prefix/'INSTALL_RECEIPT.json'),
                              'libraries': {p.name:a.digest(p) for p in sorted((prefix/'lib').glob('*.dylib'))}}
    commands = [['cmake','-S',source,'-B',output/'build','-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF',
                 '-DWITH_MYSQL=OFF','-DWITH_POSTGRESQL=OFF','-DWITH_REDIS=OFF','-DWITH_MONGODB=OFF',
                 '-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/openssl@3;/opt/homebrew/opt/libevent'],
                ['cmake','--build',output/'build','--target','turnserver','-j','4']]
    with (output/'build.log').open('w') as log:
        for command in commands: subprocess.run(list(map(str,command)),stdout=log,stderr=subprocess.STDOUT,check=True)
    matches=list((output/'build').rglob('turnserver'));matches=[p for p in matches if p.is_file()]
    a.require(len(matches)==1,'Expected one coturn executable.')
    executable=matches[0]
    record=dict(schema=1,source=lock,executable=str(executable),sha256=a.digest(executable),
                host_dependencies=dependencies,commands=[list(map(str,c)) for c in commands],
                compiler=subprocess.check_output(['clang','--version'],text=True),
                cmake=subprocess.check_output(['cmake','--version'],text=True),
                linkage=subprocess.check_output(['otool','-L',executable],text=True))
    (output/'fixture.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();print(json.dumps({'executable':build(args.output.resolve())['executable']}))
