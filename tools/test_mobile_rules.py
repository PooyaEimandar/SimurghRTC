#!/usr/bin/env python3
"""Exercise the mobile artifact validator from a real UE 5.8.3 build-rules DLL."""
import argparse
import hashlib
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
    parser.add_argument('--simulator-support', type=Path, help='Also check the current simulator support export.')
    parser.add_argument('--isolated', action='store_true', help='Also exercise the M155 C ABI artifact validator.')
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
    if args.simulator_support:
        for name in ('valid', 'changed-archive', 'missing-lock', 'retired-schema', 'wrong-version', 'resealed-wrong-lock'):
            project_root = output / ('support-' + name)
            support = project_root / 'Build/IOSSimulator'
            shutil.copytree(args.simulator_support, support)
            manifest_path = support / 'support.json'
            manifest = json.loads(manifest_path.read_text())
            if name == 'changed-archive':
                (support / 'libnghttp2.a').write_bytes(b'changed')
            elif name == 'missing-lock':
                (support / 'source-lock.json').unlink()
            elif name == 'retired-schema':
                manifest['schema'] = 1
            elif name == 'wrong-version':
                manifest['version'] = '1.64.0'
            elif name == 'resealed-wrong-lock':
                lock = support / 'source-lock.json'
                lock.write_text('{}\n')
                digest = hashlib.sha256(lock.read_bytes()).hexdigest()
                manifest['source_lock_sha256'] = digest
                manifest['files']['source-lock.json'] = digest
            manifest_path.write_text(json.dumps(manifest))
            cases.append({'name': 'simulator support: ' + name, 'validator': 'simulator-support',
                          'root': str(project_root), 'accept': name == 'valid'})
    if args.isolated:
        from test_native_artifacts import make_native_payload, seal_native
        import native_artifacts
        for target in ('Mac','Android','IOS','IOSSimulator'):
            names = ('valid','wrong-platform','corrupt-archive','resealed-header','resealed-lock','resealed-bridge','retired-schema','extra-binary')
            if target == 'Android': names += ('missing-ndk-link','failed-ndk-link','runtime-drift')
            for name in names:
                root = output / ('isolated-' + target + '-' + name)
                make_native_payload(root,target)
                selected_platform = target
                if name == 'wrong-platform': selected_platform = 'IOS' if target != 'IOS' else 'Mac'
                if name == 'corrupt-archive': (root / 'Lib/libwebrtc.a').write_bytes(b'bad')
                if name in ('resealed-header','resealed-lock'):
                    (root / ('Include/simurgh_rtc_abi.h' if name == 'resealed-header' else 'Build/source-lock.json')).write_text('changed')
                    seal_native(root)
                if name == 'resealed-bridge':
                    path = root / 'Build/build.json'; doc=json.loads(path.read_text());doc['bridge_sources']['runtime.cc']='0'*64
                    path.write_text(json.dumps(doc));seal_native(root)
                if name == 'retired-schema':
                    path=root/'artifact.json';doc=json.loads(path.read_text());doc['metadata']['schema']=1;path.write_text(json.dumps(doc))
                if name == 'extra-binary':
                    (root/'Lib/libcrypto.a').write_bytes(b'bad');seal_native(root)
                if name == 'missing-ndk-link':
                    (root/'Build/link-probe.json').unlink();seal_native(root)
                if name in ('failed-ndk-link','runtime-drift'):
                    path=root/'Build/link-probe.json';proof=json.loads(path.read_text())
                    proof['passed' if name == 'failed-ndk-link' else 'compiler_runtime_sha256'] = False if name == 'failed-ndk-link' else 'b'*64
                    path.write_text(json.dumps(proof));seal_native(root)
                cases.append({'name':'isolated ' + target + ': ' + name,'validator':'isolated','root':str(root),
                              'platform':selected_platform,'pins':str(native_artifacts.PINS),'accept':name=='valid'})
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
