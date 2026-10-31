#!/usr/bin/env python3
"""Build the pinned, isolated native backend for Unreal; export only the private C ABI."""
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
import build_native_candidate as candidate
import mobile_artifacts as artifacts
import mobile_build
ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'Config/NativeBackend'
LOCK = ROOT / 'Config/NativeWebRTC.lock.json'

def build(work, target, output, jobs=6, build_name=None, fresh=False):
    lock = artifacts.read_json(LOCK)
    inputs = {p: artifacts.digest(p) for p in [LOCK, Path(__file__), *CONFIG.glob('*')]
              if p.is_file()}
    for patch in lock['patches']:
        inputs[ROOT / patch['path']] = artifacts.digest(ROOT / patch['path'])
    artifacts.require(target in lock['platforms'], 'Unsupported native target.')
    artifacts.require(not output.exists(), 'Use a new artifact output directory.')
    spec = lock['platforms'][target]
    artifacts.require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Apple builds require Mac ARM64.')
    artifacts.require(mobile_build.run(['xcodebuild', '-version']) == lock['mac']['xcode'], 'Xcode pin mismatch.')
    artifacts.require(mobile_build.run(['xcrun', '--sdk', spec['sdk_name'], '--show-sdk-version']) == spec['sdk'], 'SDK pin mismatch.')
    with mobile_build.workspace(work) as work:
        build, empty = mobile_build.build_directory(work, 'Native' + target, build_name, fresh)
        mobile_build.free_space(work, 4)
        candidate.source_inputs(work, lock)
        host = candidate.build_tools(work, lock)
        source = work / 'src'
        probe = source / 'simurgh_probe'; probe.mkdir(exist_ok=True)
        for name in ('BUILD.gn', 'runtime.cc', 'media.cc', 'audio.cc', 'audio.h', 'signaling.cc', 'signaling.h', 'data_channels.cc', 'data_channels.h', 'simurgh_rtc_abi.h'):
            shutil.copy2(CONFIG / name, probe / name)
        (source / 'build/config/gclient_args.gni').write_text('android_ndk_version = "2@30.0.16248370"\ngenerate_location_tags = true\n')
        shutil.copy2(CONFIG / (target + '.gn'), build / 'args.gn')
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', ZERO_AR_DATE='1', SOURCE_DATE_EPOCH=str(lock['source_epoch']), LC_ALL='C', TZ='UTC')
        for name in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','SDKROOT'):
            env.pop(name, None)
        mobile_build.run([host / 'gn', 'gen', build, '--root=' + str(source)], env=env, log=build / 'gn.log')
        mobile_build.run([host / 'ninja', '-C', build, '-j', jobs, 'simurgh_probe:probe'], env=env, log=build / 'build.log')
        archive = build / 'obj/simurgh_probe/libprobe.a'
        isolated = build / 'isolated.o'
        apple_platform = {'Mac':'macos', 'IOS':'ios', 'IOSSimulator':'ios-simulator'}[target]
        mobile_build.run(['xcrun','ld','-r','-arch','arm64','-platform_version',apple_platform,spec['minimum_os'],spec['sdk'],
                          '-oso_prefix',str(build) + '/', '-exported_symbols_list',CONFIG / 'exports.txt',
                          '-all_load',archive,'-o',isolated], env=env, log=build / 'isolation.log')
        symbols = mobile_build.run(['xcrun','nm','-g','-U',isolated])
        exports = {line.split()[-1] for line in symbols.splitlines() if line.split()}
        artifacts.require(exports == set((CONFIG / 'exports.txt').read_text().split()), 'Unexpected native exports: ' + str(exports))
        artifacts.require(all(artifacts.digest(p) == digest for p, digest in inputs.items()),
                          'Build inputs changed during compilation; rebuild before exporting.')
        for directory in ('Lib','Include','Build','Licenses'):
            (output / directory).mkdir(parents=True, exist_ok=True)
        mobile_build.run(['xcrun','libtool','-static','-o',output / 'Lib/libwebrtc.a',isolated], env=env)
        mobile_build.run(['xcrun','strip','-S',output / 'Lib/libwebrtc.a'], env=env)
        shutil.copy2(CONFIG / 'simurgh_rtc_abi.h',output / 'Include/simurgh_rtc_abi.h')
        shutil.copy2(LOCK,output / 'Build/source-lock.json')
        shutil.copy2(build / 'args.gn',output / 'Build/args.gn')
        (output / 'Build/patches').mkdir()
        for patch in lock['patches']:
            shutil.copy2(ROOT / patch['path'], output / 'Build/patches' / Path(patch['path']).name)
        # Retain source license and patent inputs. Final distribution review stays SR-902.
        license_paths = ['LICENSE','PATENTS']
        for repo in lock['repositories']:
            root = work / repo
            for name in ('LICENSE','LICENSE.md','LICENSE.txt','COPYING','PATENTS'):
                if (root / name).is_file(): license_paths.append(str((root / name).relative_to(source)))
        for name in ('third_party/opus/src/COPYING','third_party/abseil-cpp/LICENSE','third_party/pffft/LICENSE','third_party/rnnoise/COPYING'):
            if (source / name).is_file(): license_paths.append(name)
        notices = '\n\n'.join('## ' + name + '\n' + (source / name).read_text(errors='replace') for name in sorted(set(license_paths)))
        (output / 'Licenses/THIRD_PARTY_NOTICES.txt').write_text(notices)
        record = {'schema':1, 'platform':target, 'source_lock_sha256':artifacts.digest(LOCK),
                  'build_directory':str(build), 'fresh_output':empty,
                  'recipe_sha256':artifacts.digest(Path(__file__)),
                  'bridge_sources':{p.name:artifacts.digest(p) for p in CONFIG.iterdir() if p.is_file()},
                  'exports':sorted(exports),'archive_before_isolation_sha256':artifacts.digest(archive),
                  'isolated_sha256':artifacts.digest(isolated), 'xcode':lock['mac']['xcode'], 'sdk':spec,
                  'compiler':mobile_build.run([source / 'third_party/llvm-build/Release+Asserts/bin/clang','--version']),
                  'runtime_validation':'unverified', 'distribution_notices':'review_required_SR902'}
        mobile_build.write_json(output / 'Build/build.json',record)
        meta = {'schema':2,'artifact_id':target.lower()+'-arm64-m155-'+artifacts.digest(LOCK)[:12],
                'platform':target,'architecture':'arm64','engine_version':'5.8.3','configuration':'Release',
                'adapter':lock['adapter'],'source_revision':lock['repositories']['src']['revision'],
                'dependencies':'isolated-boringssl-opus-vpx','abi_header_sha256':artifacts.digest(CONFIG / 'simurgh_rtc_abi.h')}
        inventory = {name:artifacts.digest(path) for name,path in artifacts.payload_files(output).items()}
        mobile_build.write_json(output / 'artifact.json', {'metadata':meta,'files':inventory})
        return {'output':str(output),'manifest_sha256':artifacts.digest(output / 'artifact.json'),'platform':target}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--platform',choices=['Mac','IOS','IOSSimulator'],required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=6)
    parser.add_argument('--build-name', help='Separate Ninja output directory under src/out.')
    parser.add_argument('--fresh', action='store_true', help='Require an empty Ninja output directory.')
    args=parser.parse_args()
    artifacts.require(1 <= args.jobs <= 16,'Use 1–16 jobs.')
    print(json.dumps(build(args.work.resolve(),args.platform,args.output.resolve(),args.jobs,args.build_name,args.fresh),indent=2))
if __name__ == '__main__': main()
