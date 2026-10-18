#!/usr/bin/env python3
"""Exercise the mobile artifact validator from a real UE 5.8.3 build-rules DLL."""
import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tests'))
from test_mobile_artifacts import make_payload, seal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--rules', type=Path, required=True, help='ContractHostModuleRules.dll from a completed UBT build.')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output must be new; existing results are never removed.')
    engine = args.engine.resolve() / 'Engine'
    runtime = {'Darwin': 'mac', 'Linux': 'linux', 'Windows': 'win'}[platform.system()]
    arch = 'arm64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x64'
    dotnet = engine / 'Binaries/ThirdParty/DotNet/10.0' / f'{runtime}-{arch}' / ('dotnet.exe' if runtime == 'win' else 'dotnet')
    if not args.rules.is_file() or not dotnet.is_file():
        parser.error('Build ContractHostEditor with UE 5.8.3 first and supply its rules DLL.')
    output.mkdir(parents=True)
    cases = []
    for target in ['Android', 'IOS', 'IOSSimulator']:
        good = output / target
        make_payload(good, target)
        seal(good, target)
        cases.append({'name': f'{target}: valid', 'root': str(good), 'platform': target, 'accept': True})
        cases.append({'name': f'{target}: wrong platform', 'root': str(good),
                      'platform': 'IOS' if target == 'Android' else 'Android', 'accept': False})
        invalid = ['header', 'library', 'extra', 'missing', 'abi', 'symlink', 'duplicate']
        if target == 'Android':
            invalid += ['relative-vtables', 'unknown-vtables']
        for name in invalid:
            bad = output / f'{target}-{name}'
            shutil.copytree(good, bad)
            if name in ('header', 'library'):
                (bad / ('Include/rtc_base/thread.h' if name == 'header' else 'Lib/libwebrtc.a')).write_text('changed')
            elif name == 'extra':
                (bad / 'unexpected.h').write_text('extra')
            elif name == 'missing':
                (bad / 'Licenses/THIRD_PARTY_NOTICES.txt').unlink()
            elif name == 'abi':
                manifest = json.loads((bad / 'artifact.json').read_text())
                manifest['metadata']['adapter'] = 'unsupported'
                (bad / 'artifact.json').write_text(json.dumps(manifest))
            elif name in ('relative-vtables', 'unknown-vtables'):
                manifest = json.loads((bad / 'artifact.json').read_text())
                toolchain = manifest['metadata']['toolchain']
                if name == 'unknown-vtables':
                    del toolchain['cpp_abi']
                else:
                    toolchain['cpp_abi'] = 'relative'
                (bad / 'artifact.json').write_text(json.dumps(manifest))
            elif name == 'symlink':
                (bad / 'outside').symlink_to(good, target_is_directory=True)
            else:
                (bad / 'artifact.json').write_text('{"metadata":{},"metadata":{}}')
            cases.append({'name': f'{target}: {name}', 'root': str(bad), 'platform': target, 'accept': False})
    cases.append({'name': 'missing artifact', 'root': str(output / 'absent'), 'platform': 'Android', 'accept': False})
    cases_path = output / 'cases.json'
    cases_path.write_text(json.dumps(cases))
    project = Path(__file__).parent / 'tests/mobile_rules/RulesProbe.csproj'
    command = [str(dotnet), 'run', '--project', str(project),
               f'-p:BaseIntermediateOutputPath={output / "obj"}/', f'-p:BaseOutputPath={output / "bin"}/', '--',
               str(args.rules.resolve()), str(engine / 'Binaries/DotNET/UnrealBuildTool'),
               str(cases_path), str(output / 'result.json')]
    with (output / 'process.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    if result.returncode:
        parser.error(f'Rules tests failed; inspect {output / "process.log"} and result.json.')
    print((output / 'result.json').read_text())


if __name__ == '__main__':
    main()
