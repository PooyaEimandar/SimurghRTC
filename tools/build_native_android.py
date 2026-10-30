#!/usr/bin/env python3
"""Build isolated M155 ARM64 in the pinned Linux container, with NDK r27c."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import tarfile
import tempfile
import zipfile
import mobile_artifacts as a
import mobile_build as b
import native_sources
ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'Config/NativeBackend'
LOCK = ROOT / 'Config/NativeWebRTC.android.lock.json'

def prepare_ndk(archive, install, sha256, *, extract):
    a.require(a.digest(archive) == sha256, 'NDK archive drift.')
    if extract and not install.exists():
        install.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='ndk-extract-', dir=install.parent) as temporary:
            staged = Path(temporary) / 'install'
            b.extract_zip(archive, staged)
            (staged / '.archive-sha256').write_text(sha256)
            staged.rename(install)
    receipt = install / '.archive-sha256'
    a.require(receipt.is_file() and receipt.read_text() == sha256,
              'NDK extraction receipt is missing or changed; re-extract the pinned archive.')
    b.verify_extraction(archive, install)

def build(work, output, ndk_cache, jobs, build_name=None, fresh=False):
    lock = a.read_json(LOCK); spec = lock['platforms']['Android']
    inputs = {p: a.digest(p) for p in [LOCK, Path(__file__), *CONFIG.glob('*')] if p.is_file()}
    for patch in lock['patches'] + lock['backend_patches']:
        inputs[ROOT / patch['path']] = a.digest(ROOT / patch['path'])
    a.require(platform.system() == 'Linux' and platform.machine() == 'x86_64', 'Use the pinned Linux x86-64 container.')
    a.require(sys.version.split()[0] == spec['python'], 'Python pin mismatch.')
    a.require(a.digest(ROOT / 'Config/NativeWebRTC/Dockerfile.android') == spec['container']['dockerfile_sha256'],
              'Container recipe drift.')
    a.require(a.digest(Path('/opt/debian-packages.txt')) == spec['container']['packages_sha256'], 'Container package drift.')
    a.require(not output.exists(), 'Use a new output directory.')
    with b.workspace(work) as work:
        build, empty = b.build_directory(work, 'NativeAndroid', build_name, fresh)
        native_sources.source_inputs(work,lock)
        host = work / 'host'; host.mkdir(exist_ok=True)
        downloads = ROOT / 'LocalBuilds/SR008/NativeDownloads'
        for name in ('gn','ninja'):
            archive = downloads / (name + '-linux.zip'); b.download(lock['tools'][name],archive)
            with zipfile.ZipFile(archive) as archive_file: (host / name).write_bytes(archive_file.read(name))
            (host / name).chmod(0o755)
        archive = downloads / 'clang-linux.tar.xz'; b.download(lock['tools']['clang'],archive)
        compiler = work / 'src/third_party/llvm-build/Release+Asserts'; compiler.mkdir(parents=True,exist_ok=True)
        with tarfile.open(archive) as archive_file: archive_file.extractall(compiler,filter='data')
        runtime_archive = downloads / 'clang-android-runtime.tar.xz'
        b.download(lock['tools']['clang_android_runtime'], runtime_archive)
        with tarfile.open(runtime_archive) as archive_file: archive_file.extractall(compiler,filter='data')
        # A read-only verified cache can be shared; a fresh workspace is self-contained.
        ndk_cache = ndk_cache or work
        ndk_archive = ndk_cache / 'downloads/android-ndk-r27c-linux.zip'
        ndk_install = ndk_cache / 'host/ndk-r27c'
        if ndk_cache == work:
            b.download(spec['ndk_archive'], ndk_archive)
        prepare_ndk(ndk_archive, ndk_install, spec['ndk_archive']['sha256'], extract=ndk_cache == work)
        ndk = ndk_install / 'android-ndk-r27c'
        a.require('Pkg.Revision = ' + spec['ndk'] in (ndk / 'source.properties').read_text(),'NDK revision mismatch.')
        source = work / 'src'; probe = source / 'simurgh_probe'; probe.mkdir(exist_ok=True)
        for name in ('BUILD.gn','runtime.cc','media.cc','signaling.cc','signaling.h','data_channels.cc','data_channels.h','simurgh_rtc_abi.h'): shutil.copy2(CONFIG / name,probe / name)
        (source / 'build/config/gclient_args.gni').write_text('android_ndk_version = "2@30.0.16248370"\ngenerate_location_tags = true\n')
        args = (CONFIG / 'Android.gn').read_text() + 'android_ndk_root = ' + json.dumps(str(ndk)) + '\n'
        (build / 'args.gn').write_text(args)
        env = dict(os.environ,PYTHONDONTWRITEBYTECODE='1',ZERO_AR_DATE='1',SOURCE_DATE_EPOCH=str(lock['source_epoch']),LC_ALL='C',TZ='UTC')
        for name in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','SDKROOT'):
            env.pop(name, None)
        b.run([host / 'gn','gen',build,'--root='+str(source)],env=env,log=build / 'gn.log')
        b.run([host / 'ninja','-C',build,'-j',jobs,'simurgh_probe:probe'],env=env,log=build / 'build.log')
        archive = build / 'obj/simurgh_probe/libprobe.a'
        isolated = build / 'isolated.o'; llvm = compiler / 'bin'
        # Modern libyuv SME routines need their compiler's runtime helpers. Keep
        # these inside the same isolated object instead of relying on UE's older NDK.
        builtins = compiler / 'lib/clang/24/lib/linux/libclang_rt.builtins-aarch64-android.a'
        b.run([llvm / 'ld.lld','-r','--whole-archive',archive,'--no-whole-archive',builtins,'-o',isolated],env=env,log=build / 'isolation.log')
        exports = {name.lstrip('_') for name in (CONFIG / 'exports.txt').read_text().split()}
        defined = b.run([llvm / 'llvm-nm','--defined-only','--extern-only','--format=posix',isolated])
        localize = build / 'localize.txt'
        localize.write_text(''.join(line.split()[0]+'\n' for line in defined.splitlines() if line.split()[0] not in exports))
        b.run([llvm / 'llvm-objcopy','--localize-symbols='+str(localize),isolated],env=env)
        symbols = b.run([llvm / 'llvm-nm','--defined-only','--extern-only','--format=posix',isolated])
        a.require({line.split()[0] for line in symbols.splitlines()} == exports,'Unexpected Android exports.')
        consumer = build / 'abi-consumer'; consumer.mkdir(exist_ok=True)
        ndk_bin = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
        b.run([ndk_bin / 'clang','--target=aarch64-none-linux-android26','-std=c11','-I',CONFIG,
               '-c',CONFIG / 'abi_test.c','-o',consumer / 'abi_test.o'],env=env,log=consumer / 'link.log')
        b.run([ndk_bin / 'clang++','--target=aarch64-none-linux-android26','-static-libstdc++',
               consumer / 'abi_test.o',isolated,'-Wl,-Bsymbolic','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
               '-llog','-landroid','-lm','-o',consumer / 'abi_test'],env=env,log=consumer / 'link.log')
        headers = b.run([llvm / 'llvm-readelf','-lW',consumer / 'abi_test'])
        loads = [line.split() for line in headers.splitlines() if line.strip().startswith('LOAD')]
        a.require(loads and all(int(line[-1],16) >= 16384 for line in loads), 'NDK consumer is not 16 KiB aligned.')
        a.require(all(a.digest(p) == digest for p, digest in inputs.items()),
                  'Build inputs changed during compilation; rebuild before exporting.')
        for name in ('Lib','Include','Build','Licenses'): (output / name).mkdir(parents=True,exist_ok=True)
        b.run([llvm / 'llvm-ar','rcsD',output / 'Lib/libwebrtc.a',isolated],env=env)
        b.run([llvm / 'llvm-strip','--strip-debug',output / 'Lib/libwebrtc.a'],env=env)
        shutil.copy2(CONFIG / 'simurgh_rtc_abi.h',output / 'Include/simurgh_rtc_abi.h')
        shutil.copy2(LOCK,output / 'Build/source-lock.json');shutil.copy2(build / 'args.gn',output / 'Build/args.gn')
        shutil.copy2('/opt/debian-packages.txt',output / 'Build/debian-packages.txt')
        b.write_json(output / 'Build/link-probe.json', {
            'passed':True, 'compiler':b.run([ndk_bin / 'clang++','--version']),
            'minimum_api':26, 'load_alignment':16384, 'runtime_validation':'unverified',
            'executable_sha256':a.digest(consumer / 'abi_test'), 'compiler_runtime_sha256':a.digest(builtins)})
        for patch in lock['patches'] + lock['backend_patches']:
            dest = output / 'Build/patches' / Path(patch['path']).name;dest.parent.mkdir(exist_ok=True);shutil.copy2(ROOT / patch['path'],dest)
        licenses=[]
        for repo in lock['repositories']:
            for name in ('LICENSE','LICENSE.md','LICENSE.txt','COPYING','PATENTS'):
                path=work / repo / name
                if path.is_file(): licenses.append(path)
        for name in ('third_party/opus/src/COPYING','third_party/abseil-cpp/LICENSE','third_party/pffft/LICENSE','third_party/rnnoise/COPYING'):
            if (source / name).is_file(): licenses.append(source / name)
        (output / 'Licenses/THIRD_PARTY_NOTICES.txt').write_text('\n\n'.join('## '+str(p.relative_to(source))+'\n'+p.read_text(errors='replace') for p in sorted(set(licenses))))
        record={'schema':1,'platform':'Android','source_lock_sha256':a.digest(LOCK),'recipe_sha256':a.digest(Path(__file__)),
                'build_directory':str(build),'fresh_output':empty,
                'bridge_sources':{p.name:a.digest(p) for p in CONFIG.iterdir() if p.is_file()},'exports':sorted(exports),
                'compiler':b.run([llvm / 'clang','--version']),'ndk':spec['ndk'],'minimum_os':spec['minimum_os'],
                'compiler_runtime_sha256':a.digest(builtins),
                'archive_before_isolation_sha256':a.digest(archive),'isolated_sha256':a.digest(isolated),'runtime_validation':'unverified',
                'distribution_notices':'review_required_SR902'}
        b.write_json(output / 'Build/build.json',record)
        meta={'schema':2,'artifact_id':'android-arm64-m155-'+a.digest(LOCK)[:12],'platform':'Android','architecture':'arm64','engine_version':'5.8.3',
              'configuration':'Release','adapter':lock['adapter'],'source_revision':lock['repositories']['src']['revision'],
              'dependencies':'isolated-boringssl-opus-vpx','abi_header_sha256':a.digest(CONFIG / 'simurgh_rtc_abi.h')}
        b.write_json(output / 'artifact.json',{'metadata':meta,'files':{n:a.digest(p) for n,p in a.payload_files(output).items()}})
        return {'output':str(output),'manifest_sha256':a.digest(output / 'artifact.json')}
if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ndk-cache',type=Path,help='Optional verified r27c workspace; otherwise download into --work.')
    parser.add_argument('--jobs',type=int,default=6)
    parser.add_argument('--build-name', help='Separate Ninja output directory under src/out.')
    parser.add_argument('--fresh', action='store_true', help='Require an empty Ninja output directory.')
    args=parser.parse_args();a.require(1<=args.jobs<=16,'Use 1–16 jobs.')
    print(json.dumps(build(args.work.resolve(),args.output.resolve(),args.ndk_cache.resolve() if args.ndk_cache else None,args.jobs,args.build_name,args.fresh),indent=2))
