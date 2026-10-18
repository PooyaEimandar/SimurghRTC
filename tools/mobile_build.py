#!/usr/bin/env python3
"""Build the M0 native mobile probe dependency from checked, pinned inputs."""
from __future__ import annotations

import argparse
import contextlib
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import html
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

import mobile_artifacts as artifacts

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'Config/MobileWebRTC.lock.json'
RECIPE_SHA256 = artifacts.digest(Path(__file__))


def require(ok, message):
    if not ok:
        raise ValueError(message)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def run(command, *, cwd=None, env=None, log=None):
    command = [str(value) for value in command]
    if log:
        with log.open('a') as stream:
            stream.write(json.dumps(command) + '\n')
            stream.flush()
            result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
            require(result.returncode == 0, f'{Path(command[0]).name} failed; see {log}')
        return ''
    return subprocess.check_output(command, cwd=cwd, env=env, text=True, stderr=subprocess.PIPE).strip()


def load_lock():
    lock = artifacts.read_json(LOCK)
    require(lock['schema'] == 1 and lock['adapter'] == artifacts.ADAPTER, 'Unsupported build lock.')
    for name, spec in lock['repositories'].items():
        artifacts.relative_path(name)
        require(len(spec['revision']) == 40 and all(c in '0123456789abcdef' for c in spec['revision']),
                f'Unpinned source: {name}')
        require(spec['url'].startswith('https://'), 'Source URLs must use HTTPS.')
    for patch in lock['patches']:
        path = ROOT / artifacts.relative_path(patch['path'])
        require(artifacts.digest(path) == patch['sha256'], f'Patch checksum mismatch: {path}')
    require(artifacts.digest(ROOT / 'Config/MobileWebRTC/Dockerfile')
            == lock['platforms']['Android']['container']['dockerfile_sha256'], 'Container recipe checksum mismatch.')
    require(artifacts.digest(ROOT / 'Config/MobileWebRTC/android_ndk.gn')
            == lock['platforms']['Android']['ndk_gn_sha256'], 'NDK GN overlay checksum mismatch.')
    return lock


def free_space(path, gib):
    require(shutil.disk_usage(path).free >= gib * 1024**3,
            f'Need at least {gib} GiB free at {path}. Use a larger build volume.')


def owned_path(work, relative):
    path = work
    for part in PurePosixPath(artifacts.relative_path(str(relative))).parts:
        path /= part
        require(not path.is_symlink(), f'Symlink in generated workspace path: {path}')
    return path


@contextlib.contextmanager
def workspace(path):
    path = path.absolute()
    require(not path.is_symlink(), 'Build workspace cannot be a symlink.')
    if not path.exists():
        path.mkdir(parents=True)
    marker = path / '.simurghrtc-mobile-workspace.json'
    if not marker.exists():
        require(not any(path.iterdir()), 'Use a new workspace; existing directories are never adopted or reset.')
        write_json(marker, {'schema': 1, 'purpose': 'SimurghRTC mobile dependency builds'})
    require(artifacts.read_json(marker) == {'schema': 1, 'purpose': 'SimurghRTC mobile dependency builds'},
            'Invalid workspace marker.')
    with (path / '.build.lock').open('a') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('Another operation owns this build workspace.') from error
        yield path.resolve()


def verify_source(work, lock):
    expected_changes = {}
    for patch in lock['patches']:
        expected_changes.setdefault(patch['repository'], {}).update(patch['files'])
    def verify_repository(item):
        name, spec = item
        path = owned_path(work, name)
        require((path / '.git').is_dir() and not (path / '.git').is_symlink(),
                f'Missing pinned repository: {name}. Run fetch.')
        require(run(['git', 'rev-parse', 'HEAD'], cwd=path) == spec['revision'], f'Source revision drift: {name}')
        require(run(['git', 'remote', 'get-url', 'origin'], cwd=path) == spec['url'], f'Source remote drift: {name}')
        changed = set(filter(None, run(['git', 'diff', '--name-only', 'HEAD'], cwd=path).splitlines()))
        allowed = expected_changes.get(name, {})
        require(changed <= set(allowed), f'Unexpected source edits in {name}: {sorted(changed - set(allowed))}')
        for filename, hashes in allowed.items():
            require(artifacts.digest(path / filename) in hashes.values(), f'Edited patch input: {name}/{filename}')
        generated = {
            'src': {'out/', 'simurgh_external/'},
            'src/build': {'config/gclient_args.gni'},
            'src/buildtools': {'mac/gn', 'linux64/gn'},
            'src/third_party': {'android_ndk/'},
        }.get(name, set())
        if name == 'src/third_party' and (path / 'android_ndk').exists():
            overlay = tree_inventory(path / 'android_ndk')
            expected = {'BUILD.gn': lock['platforms']['Android']['ndk_gn_sha256'],
                        'NOTICE': lock['platforms']['Android']['ndk_notice_sha256']}
            require(all(key in expected and expected[key] == sha for key, sha in overlay.items()), 'NDK GN overlay drift.')
        nested = {child[len(name) + 1:] + '/' for child in lock['repositories'] if child.startswith(name + '/')}
        untracked = set(filter(None, run(['git', 'ls-files', '--others', '--directory', '-z'], cwd=path).split('\0')))
        require(untracked <= generated | nested,
                f'Unexpected source files in {name}: {sorted(untracked - generated - nested)}')
    with ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(verify_repository, lock['repositories'].items()))


def fetch(work, lock):
    free_space(work, 20)
    for name in sorted(lock['repositories'], key=lambda value: (value.count('/'), value)):
        spec = lock['repositories'][name]
        path = work / name
        if (path / '.git').exists():
            require(run(['git', 'remote', 'get-url', 'origin'], cwd=path) == spec['url'], f'Wrong repository: {name}')
            try:
                head = run(['git', 'rev-parse', 'HEAD'], cwd=path)
            except subprocess.CalledProcessError:
                head = None  # Resume a failed initial fetch only.
            if head:
                require(head == spec['revision'], f'Existing checkout has another revision: {name}; use a new workspace.')
                continue
        else:
            require(not path.exists() or not any(path.iterdir()), f'Refusing to replace nonempty {path}')
            path.mkdir(parents=True, exist_ok=True)
            run(['git', 'init', '-q', path])
            run(['git', 'remote', 'add', 'origin', spec['url']], cwd=path)
        print(f'Fetching {name} at {spec["revision"][:12]}', flush=True)
        run(['git', 'fetch', '--depth=1', 'origin', spec['revision']], cwd=path)
        run(['git', 'checkout', '--detach', 'FETCH_HEAD'], cwd=path)
    verify_source(work, lock)


def apply_patches(work, lock):
    verify_source(work, lock)
    for patch in lock['patches']:
        repo = work / patch['repository']
        states = [artifacts.digest(repo / name) == hashes['after'] for name, hashes in patch['files'].items()]
        if all(states):
            continue
        require(not any(states), f'Partially applied patch: {patch["path"]}')
        run(['git', 'apply', '--check', ROOT / patch['path']], cwd=repo)
        run(['git', 'apply', ROOT / patch['path']], cwd=repo)
        for name, hashes in patch['files'].items():
            require(artifacts.digest(repo / name) == hashes['after'], f'Patch result mismatch: {name}')


def download(spec, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        require(artifacts.digest(destination) == spec['sha256'], f'Cached download is corrupt: {destination}')
        return
    temporary = destination.with_suffix(destination.suffix + '.part')
    with urllib.request.urlopen(spec['url'], timeout=120) as response, temporary.open('wb') as output:
        shutil.copyfileobj(response, output, 1024 * 1024)
    require(artifacts.digest(temporary) == spec['sha256'], f'Download checksum mismatch: {destination}')
    temporary.replace(destination)


def extract_zip(archive, destination):
    """Toolchain archives may contain relative internal symlinks, but never escapes."""
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as source:
        entries = source.infolist()
        for item in entries:
            name = item.filename.rstrip('/')
            require(name and not name.startswith('/') and '\\' not in name
                    and all(part not in ('', '.', '..') for part in name.split('/')),
                    'Unsafe archive path.')
            target = destination / name
            require(destination.resolve() in target.resolve().parents, 'Archive escapes its output directory.')
            mode = item.external_attr >> 16
            if stat.S_ISLNK(mode):
                link = source.read(item).decode('utf-8')
                require(not PurePosixPath(link).is_absolute(), 'Absolute archive symlink.')
                resolved = (target.parent / link).resolve()
                require(resolved == destination.resolve() or destination.resolve() in resolved.parents,
                        'Archive symlink escapes its output directory.')
        # Create symlinks after ordinary files to avoid writing through archive-created links.
        for links in (False, True):
            for item in entries:
                mode = item.external_attr >> 16
                if stat.S_ISLNK(mode) != links:
                    continue
                target = destination / item.filename.rstrip('/')
                if item.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                require(not target.exists() and not target.is_symlink(), f'Archive would overwrite {target}')
                if links:
                    target.symlink_to(source.read(item).decode('utf-8'))
                else:
                    with source.open(item) as incoming, target.open('wb') as output:
                        shutil.copyfileobj(incoming, output)
                    target.chmod(mode & 0o777 if mode & 0o777 else 0o644)
        for item in entries:
            target = destination / item.filename.rstrip('/')
            resolved = target.resolve()
            require(resolved == destination.resolve() or destination.resolve() in resolved.parents,
                    'Archive symlink chain escapes its output directory.')


def verify_extraction(archive, destination):
    """Compare cached toolchain contents with the already checksummed archive."""
    expected = set()
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if item.is_dir():
                continue
            expected.add(item.filename)
            path = destination / item.filename
            mode = item.external_attr >> 16
            if stat.S_ISLNK(mode):
                require(path.is_symlink() and os.readlink(path) == source.read(item).decode(),
                        f'Toolchain symlink drift: {path}')
            else:
                require(path.is_file() and not path.is_symlink(), f'Missing toolchain file: {path}')
                digest = hashlib.sha256()
                with source.open(item) as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(block)
                require(artifacts.digest(path) == digest.hexdigest(), f'Toolchain file drift: {path}')
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file() or p.is_symlink()}
    require(actual == expected | {'.archive-sha256'}, 'Unexpected files in cached toolchain.')


def tool(work, lock, name, host):
    key = f'{name}-{host}'
    spec = lock['tools'][key]
    archive = work / 'downloads' / f'{key}.zip'
    download(spec, archive)
    # Re-extract into a fresh temporary location. No unverified cached executable is reused.
    directory = Path(tempfile.mkdtemp(prefix=key + '-', dir=work / 'host'))
    extract_zip(archive, directory)
    executable = directory / name
    executable.chmod(0o755)
    return executable


def tree_inventory(root):
    require(root.is_dir() and not root.is_symlink(), f'Missing input directory: {root}')
    result = {}
    for path in sorted(root.rglob('*')):
        require(not path.is_symlink(), f'Symlink in engine headers: {path}')
        if path.is_file():
            result[path.relative_to(root).as_posix()] = artifacts.digest(path)
    require(result, f'Empty input directory: {root}')
    return result


def tree_digest(inventory):
    return hashlib.sha256(json.dumps(inventory, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def stage_headers(work, lock, engine, target):
    root = owned_path(work, 'src/simurgh_external')
    selected = {'openssl': 'openssl-' + target.lower(), 'opus': 'opus', 'vpx': 'vpx'}
    records = {}
    for name, key in selected.items():
        spec = lock['engine_headers'][key]
        source = engine / spec['path']
        inventory = tree_inventory(source)
        require(tree_digest(inventory) == spec['tree_sha256'], f'Engine header drift: {key}')
        destination = owned_path(work, 'src/simurgh_external/' + name)
        # This generated directory belongs to the marked workspace. Do not touch the engine.
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(source, destination)
        require(tree_digest(tree_inventory(destination)) == spec['tree_sha256'], f'Header copy mismatch: {key}')
        records[key] = spec['tree_sha256']
    return records


def preflight(lock, target, python):
    expected = lock['platforms'][target]
    require(platform.system() == expected['host'] and platform.machine() == expected['machine'],
            f'{target} requires {expected["host"]} {expected["machine"]}. See docs/MOBILE_BUILDS.md.')
    actual_python = run([python, '-c', 'import platform;print(platform.python_version())'])
    require(actual_python == expected['python'], f'Expected Python {expected["python"]}, found {actual_python}.')
    record = {'python': actual_python, 'host': platform.platform()}
    if target == 'Android':
        packages = Path('/opt/debian-packages.txt')
        require(packages.is_file() and artifacts.digest(packages) == expected['container']['packages_sha256'],
                'Use the pinned Android container; its package manifest differs or is missing.')
        record['container_packages_sha256'] = artifacts.digest(packages)
    if target == 'IOS':
        for command, field in [(['xcodebuild', '-version'], 'xcode'),
                               (['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'], 'sdk'),
                               (['xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version'], 'sdk_build')]:
            actual = run(command)
            require(actual == expected[field], f'iOS {field} differs from the lock: {actual}')
            record[field] = actual
        compiler = Path(run(['xcrun', '--find', 'clang']))
        record['compiler'] = run([compiler, '--version']).splitlines()[0]
        require(record['compiler'] == expected['compiler'], 'Xcode compiler differs from the lock.')
        record['compiler_root'] = str(compiler.parent.parent)
    return record


def gn_arguments(lock, target, compiler_root, ndk=None):
    result = dict(lock['common_gn_args'])
    result.update(target_os='ios' if target == 'IOS' else 'android',
                  clang_base_path=str(compiler_root), rtc_ssl_root='//simurgh_external/openssl')
    if target == 'IOS':
        result.update(target_environment='device', ios_enable_code_signing=False,
                      ios_deployment_target=lock['platforms'][target]['minimum_os'], use_lld=False,
                      enable_dsyms=False, enable_stripping=False)
    else:
        result.update(android_ndk_root=str(ndk), android_ndk_version='r27c', android_ndk_major_version=27,
                      android64_ndk_api_level=int(lock['platforms'][target]['minimum_os']),
                      default_min_sdk_version=int(lock['platforms'][target]['minimum_os']),
                      use_sysroot=False)
    return '\n'.join(f'{key} = {json.dumps(value)}' for key, value in sorted(result.items())) + '\n'


def prepare(work, lock, target, python, engine, build_name=None):
    free_space(work, 12)
    record = preflight(lock, target, python)
    if target == 'Android':
        with tempfile.TemporaryDirectory(prefix='case-check-', dir=work) as directory:
            (Path(directory) / 'lowercase').touch()
            require(not (Path(directory) / 'LOWERCASE').exists(),
                    'Android requires a case-sensitive Linux workspace. Use the documented Docker volume.')
    apply_patches(work, lock)
    record['engine_headers'] = stage_headers(work, lock, engine, target)
    owned_path(work, 'host').mkdir(exist_ok=True)
    owned_path(work, 'downloads').mkdir(exist_ok=True)
    host = 'mac-arm64' if target == 'IOS' else 'linux-amd64'
    gn = tool(work, lock, 'gn', host)
    ninja = tool(work, lock, 'ninja', host)
    bindir = Path(tempfile.mkdtemp(prefix='python-', dir=work / 'host'))
    (bindir / 'python3').symlink_to(python.resolve())
    env = dict(os.environ)
    for key in ['CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'SDKROOT',
                'PYTHONPATH', 'PYTHONHOME']:
        env.pop(key, None)
    env.update(PATH=str(bindir) + os.pathsep + '/usr/bin:/bin:/usr/sbin:/sbin',
               DEPOT_TOOLS_UPDATE='0', DEPOT_TOOLS_WIN_TOOLCHAIN='0',
               PYTHONDONTWRITEBYTECODE='1',
               SOURCE_DATE_EPOCH=str(lock['source_epoch']), ZERO_AR_DATE='1', LC_ALL='C', TZ='UTC')
    compiler_root = Path(record.get('compiler_root', '/missing'))
    ndk = None
    if target == 'Android':
        spec = lock['platforms'][target]['ndk_archive']
        archive = work / 'downloads/android-ndk-r27c-linux.zip'
        download(spec, archive)
        # The marker detects incomplete extraction; file hashes detect later edits.
        install = owned_path(work, 'host/ndk-r27c')
        if not install.exists():
            with tempfile.TemporaryDirectory(prefix='ndk-extract-', dir=work / 'host') as temporary:
                staged = Path(temporary) / 'install'
                extract_zip(archive, staged)
                (staged / '.archive-sha256').write_text(spec['sha256'])
                staged.rename(install)
        require((install / '.archive-sha256').read_text() == spec['sha256'], 'NDK extraction is incomplete.')
        verify_extraction(archive, install)
        ndk = install / 'android-ndk-r27c'
        require(f'Pkg.Revision = {lock["platforms"][target]["ndk"]}' in (ndk / 'source.properties').read_text(),
                'Wrong NDK revision.')
        compiler_root = ndk / 'toolchains/llvm/prebuilt/linux-x86_64'
        record['compiler'] = run([compiler_root / 'bin/clang', '--version']).splitlines()[0]
        require(f'clang version {lock["platforms"][target]["compiler_version"]}' in record['compiler']
                and lock['platforms'][target]['llvm_revision'] in record['compiler'], 'Unexpected NDK compiler.')
        record['sdk'] = lock['platforms'][target]['ndk']
        record['compiler_root'] = str(compiler_root)
        ndk_overlay = owned_path(work, 'src/third_party/android_ndk')
        ndk_overlay.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'Config/MobileWebRTC/android_ndk.gn', ndk_overlay / 'BUILD.gn')
        require(artifacts.digest(ndk / 'NOTICE') == lock['platforms']['Android']['ndk_notice_sha256'], 'NDK notice drift.')
        shutil.copy2(ndk / 'NOTICE', ndk_overlay / 'NOTICE')
    src = work / 'src'
    (src / 'build/config/gclient_args.gni').write_text('generate_location_tags = true\n')
    # Upstream's license generator resolves GN through this path.
    upstream_gn = src / ('buildtools/mac/gn' if target == 'IOS' else 'buildtools/linux64/gn')
    upstream_gn.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(gn, upstream_gn)
    name = build_name or target
    require(name.isascii() and name.replace('-', '').replace('_', '').isalnum(), 'Invalid build directory name.')
    build = owned_path(work, 'src/out/' + name)
    build.mkdir(parents=True, exist_ok=True)
    (build / 'args.gn').write_text(gn_arguments(lock, target, compiler_root, ndk))
    record['lock_sha256'] = artifacts.digest(LOCK)
    record['recipe_sha256'] = RECIPE_SHA256
    record['gn'] = run([gn, '--version'])
    record['ninja'] = run([ninja, '--version'])
    write_json(build / 'toolchain.json', record)
    return src, build, gn, ninja, env, record


def link_probe(work, lock, target, build, env, record, engine):
    libraries = []
    for spec in lock['platforms'][target]['link_libraries']:
        path = engine / spec['path']
        require(path.is_file() and artifacts.digest(path) == spec['sha256'],
                f'Engine link dependency drift: {path}')
        libraries.append(path)
    source = ROOT / 'Config/MobileWebRTC/link_probe.cc'
    library = build / 'obj/libwebrtc.a'
    if target == 'IOS':
        symbols = run(['xcrun', 'nm', '-gUj', library]).splitlines()
        symbols = [symbol[1:] if symbol.startswith('_') else symbol for symbol in symbols]
    else:
        symbols = run([Path(record['compiler_root']) / 'bin/llvm-nm',
                       '--defined-only', '--extern-only', '--format=just-symbols', library]).splitlines()
    prefixes = ('opus_', 'vpx_', 'SSL_', 'OPENSSL_', 'BORINGSSL_', 'CRYPTO_', 'EVP_', 'BIO_',
                'AES_', 'RSA_', 'EC_', 'BN_', 'ASN1_', 'X509_', 'HMAC_', 'SHA256_', 'PKCS')
    embedded = sorted(symbol for symbol in symbols if not symbol.endswith(':') and symbol.startswith(prefixes))
    require(not embedded, f'Archive unexpectedly embeds external codec/crypto symbols: {embedded[:20]}')
    compiler = Path(record['compiler_root']) / 'bin/clang++'
    command = [compiler, '-std=c++20', '-fno-rtti', '-fno-exceptions', '-fPIC',
               '-DWEBRTC_POSIX=1', '-DABSL_ALLOCATOR_NOTHROW=1',
               '-I' + str(work / 'src'), '-I' + str(work / 'src/third_party/abseil-cpp'), source]
    if target == 'IOS':
        binary = build / 'link-probe.dylib'
        command += ['-target', 'arm64-apple-ios' + lock['platforms'][target]['minimum_os'],
                    '-isysroot', run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']),
                    '-DWEBRTC_MAC=1', '-DWEBRTC_IOS=1', '-dynamiclib',
                    '-Wl,-undefined,error', '-Wl,-force_load,' + str(library), *libraries]
        for framework in ['Foundation', 'CoreFoundation', 'Security', 'AudioToolbox', 'CoreAudio',
                          'CoreMedia', 'CoreVideo', 'VideoToolbox']:
            command += ['-framework', framework]
    else:
        binary = build / 'link-probe.so'
        exports = build / 'link-probe.exports'
        exports.write_text('{ global: SimurghRTCLinkProbe; local: *; };\n')
        command += ['--target=aarch64-linux-android' + lock['platforms'][target]['minimum_os'],
                    '-DWEBRTC_ANDROID=1', '-shared', '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384',
                    '-static-libstdc++', '-Wl,-Bsymbolic', '-Wl,--version-script=' + str(exports),
                    '-Wl,--whole-archive', library, '-Wl,--no-whole-archive',
                    '-Wl,--start-group', *libraries, '-Wl,--end-group', '-llog', '-landroid', '-ldl', '-lm']
    run(command + ['-o', binary], env=env, log=build / 'link-probe.log')
    result = {'status': 'linked', 'whole_archive': True, 'runtime_validation': 'unverified',
              'external_codec_crypto_definitions': embedded,
              'probe_sha256': artifacts.digest(source), 'binary_sha256': artifacts.digest(binary),
              'engine_libraries': lock['platforms'][target]['link_libraries']}
    if target == 'Android':
        result['cpp_linkage'] = 'static libc++; Unreal-style local symbol binding'
        readelf = Path(record['compiler_root']) / 'bin/llvm-readelf'
        segments = run([readelf, '--program-headers', '--wide', binary])
        loads = [line.split() for line in segments.splitlines() if line.strip().startswith('LOAD ')]
        require(loads and all(int(row[-1], 16) >= 16384 for row in loads), 'Probe lacks 16 KiB LOAD alignment.')
        result['load_alignment_bytes'] = [int(row[-1], 16) for row in loads]
    write_json(build / 'link-probe.json', result)
    return result


def export_artifact(work, lock, target, build, gn, env, record, output):
    require(artifacts.digest(LOCK) == record['lock_sha256'], 'Build lock changed during compilation; rerun build.')
    library = build / 'obj/libwebrtc.a'
    require(library.is_file(), 'GN did not produce libwebrtc.a.')
    artifacts.validate_archive(library, target)
    symbols = output.with_name(output.name + '.symbols')
    require(not output.exists() and not symbols.exists(), 'Artifact and symbol outputs must be new.')
    with tempfile.TemporaryDirectory(prefix='export-', dir=work) as temporary:
        payload = Path(temporary) / 'payload'
        (payload / 'Lib').mkdir(parents=True)
        delivered = payload / 'Lib/libwebrtc.a'
        shutil.copy2(library, delivered)
        if target == 'IOS':
            run(['xcrun', 'strip', '-S', delivered], env=env)
        else:
            run([Path(record['compiler_root']) / 'bin/llvm-strip', '--strip-debug', delivered], env=env)
        # Export WebRTC headers plus the exact dependent public header trees.
        src = work / 'src'
        headers = []
        for repository, prefix in [('src', ''),
                                   ('src/third_party', 'third_party'),
                                   ('src/third_party/libyuv', 'third_party/libyuv'),
                                   ('src/third_party/libsrtp', 'third_party/libsrtp'),
                                   ('src/third_party/jsoncpp/source', 'third_party/jsoncpp/source')]:
            names = run(['git', 'ls-files', '-z'], cwd=work / repository).split('\0')
            for name in names:
                if Path(name).suffix not in ('.h', '.hpp', '.inc'):
                    continue
                if repository == 'src/third_party' and not name.startswith(('abseil-cpp/', 'pffft/', 'rnnoise/')):
                    continue
                headers.append((work / repository / name, PurePosixPath(prefix) / name))
        for header in (build / 'gen').rglob('*.h'):
            headers.append((header, PurePosixPath(header.relative_to(build / 'gen').as_posix())))
        for source, relative in headers:
            destination = payload / 'Include' / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        records = payload / 'Build'
        records.mkdir()
        shutil.copy2(build / 'args.gn', records / 'args.gn')
        shutil.copy2(build / 'toolchain.json', records / 'toolchain.json')
        shutil.copy2(build / 'link-probe.json', records / 'link-probe.json')
        shutil.copy2(LOCK, records / 'build-lock.json')
        dependencies = {name: spec['revision'] for name, spec in lock['repositories'].items() if name != 'src'}
        dependencies.update(record['engine_headers'])
        dependencies.update({name: spec['sha256'] for name, spec in lock['tools'].items()})
        dependencies['build-lock'] = artifacts.digest(LOCK)
        dependencies['build-runner'] = record['recipe_sha256']
        dependencies['link-probe'] = artifacts.digest(ROOT / 'Config/MobileWebRTC/link_probe.cc')
        if target == 'Android':
            dependencies['ndk-archive'] = lock['platforms'][target]['ndk_archive']['sha256']
            dependencies['ndk-gn-overlay'] = lock['platforms'][target]['ndk_gn_sha256']
            shutil.copy2(ROOT / 'Config/MobileWebRTC/android_ndk.gn', records / 'android_ndk.gn')
            shutil.copy2('/opt/debian-packages.txt', records / 'debian-packages.txt')
        patches = []
        for patch in lock['patches']:
            destination = records / 'patches' / Path(patch['path']).name
            destination.parent.mkdir(exist_ok=True)
            shutil.copy2(ROOT / patch['path'], destination)
            patches.append(destination.relative_to(payload).as_posix())
        write_json(records / 'source-lock.json', {'webrtc_revision': lock['repositories']['src']['revision'],
                                                'dependencies': dependencies, 'patches': patches})
        (records / 'recipe.txt').write_text('Built with tools/mobile_build.py and the accompanying build-lock.json.\n'
            'Target: //:webrtc; Release ARM64, external Unreal OpenSSL/Opus/VPX.\n'
            'GN arguments and exact toolchain observations are in args.gn and toolchain.json.\n'
            'Runtime validation: unverified; this is an M0 native-probe dependency.\n')
        licenses = payload / 'Licenses'
        licenses.mkdir()
        license_env = dict(env, SIMURGHRTC_GN=str(gn))
        run([env['PATH'].split(os.pathsep)[0] + '/python3', src / 'tools_webrtc/libs/generate_licenses.py',
             '--target', '//:webrtc', licenses, build], cwd=src, env=license_env, log=build / 'licenses.log')
        # Header bundles include dependencies outside the selected link graph.
        # Preserve their notices too; this does not claim their code is linked.
        notices = html.unescape((licenses / 'LICENSE.md').read_text())
        for name, relative in [
                ('JSONCPP', 'third_party/jsoncpp/LICENSE'),
                ('PORTAUDIO', 'modules/third_party/portaudio/LICENSE'),
                ('SOCKETROCKET', 'examples/objc/AppRTCMobile/third_party/SocketRocket/LICENSE')]:
            notice = src / relative
            shutil.copy2(notice, licenses / (name + '-LICENSE.txt'))
            notices += '\n# ' + name + ' exported headers\n' + notice.read_text()
        (licenses / 'THIRD_PARTY_NOTICES.txt').write_text(notices)
        shutil.copy2(src / 'PATENTS', licenses / 'WEBRTC-PATENTS.txt')
        shutil.copy2(src / 'third_party/libyuv/PATENTS', licenses / 'LIBYUV-PATENTS.txt')
        metadata = Path(temporary) / 'metadata.json'
        write_json(metadata, {'schema': 1, 'artifact_id': f'{target.lower()}-arm64-{lock["repositories"]["src"]["revision"][:12]}-{record["lock_sha256"][:12]}',
                              'platform': target, 'architecture': 'arm64', 'engine_version': lock['engine_version'],
                              'adapter': lock['adapter'], 'configuration': 'Release',
                              'dependencies': 'unreal-openssl-opus-vpx', 'source_revision': lock['repositories']['src']['revision'],
                              'toolchain': {'host': 'Mac' if target == 'IOS' else 'Linux',
                                            'compiler': record['compiler'], 'sdk': record['sdk'],
                                            'minimum_os': lock['platforms'][target]['minimum_os'], 'cpp_runtime': 'libc++'},
                              'codecs': {'audio': ['opus', 'ISAC', 'G722', 'PCMU', 'PCMA', 'multiopus', 'L16'],
                                         'video': ['VP8', 'VP9']}})
        result = artifacts.package(payload, metadata, output)
    symbols.mkdir(parents=True)
    shutil.copy2(library, symbols / 'libwebrtc.unstripped.a')
    write_json(symbols / 'manifest.json', {'archive_sha256': artifacts.digest(library),
                                         'artifact_manifest_sha256': result['manifest_sha256'],
                                         'kind': 'unstripped static library; app dSYMs are produced when linking the app'})
    return result


def compare_artifacts(first, second):
    require(first.resolve() != second.resolve(), 'Compare two different artifact directories.')
    for directory in (first, second):
        artifacts.validate(directory)
        require((directory / 'Build/build-lock.json').is_file(), 'Repeat checks require the full build lock.')
    documents = [artifacts.read_json(directory / artifacts.MANIFEST) for directory in (first, second)]
    require(documents[0]['metadata'] == documents[1]['metadata'], 'Artifact identities/toolchains differ.')
    selected = lambda document: {name: sha for name, sha in document['files'].items()
                                if name.startswith(('Lib/', 'Include/', 'Licenses/', 'Build/patches/'))
                                or name in ('Build/build-lock.json', 'Build/source-lock.json')}
    a, b = map(selected, documents)
    differences = sorted(name for name in a.keys() | b.keys() if a.get(name) != b.get(name))
    require(not differences, f'Build outputs differ: {differences}')
    return {'status': 'identical', 'compared_files': len(a),
            'archive_sha256': a['Lib/libwebrtc.a'], 'runtime_validation': 'unverified'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['plan', 'fetch', 'configure', 'build', 'compare'])
    parser.add_argument('--platform', choices=['Android', 'IOS'], default='IOS')
    parser.add_argument('--work', type=Path)
    parser.add_argument('--build-name', help='Fresh output directory under src/out, for repeat-build checks.')
    parser.add_argument('--first', type=Path)
    parser.add_argument('--second', type=Path)
    parser.add_argument('--engine', type=Path)
    parser.add_argument('--python', type=Path, default=Path(sys.executable))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    try:
        lock = load_lock()
        if args.command == 'compare':
            require(args.first is not None and args.second is not None, '--first and --second are required.')
            print(json.dumps(compare_artifacts(args.first, args.second), indent=2))
            return
        require(args.work is not None, '--work is required.')
        if args.command == 'plan':
            print(json.dumps({'source': lock['repositories']['src'], 'repositories': len(lock['repositories']),
                              'toolchain': lock['platforms'][args.platform], 'arguments': lock['common_gn_args'],
                              'workspace': str(args.work.absolute()), 'runtime_validation': 'unverified'}, indent=2))
            return
        require(1 <= args.jobs <= 32, 'Use between 1 and 32 compiler jobs.')
        if args.command in ('configure', 'build'):
            require(args.engine is not None, '--engine is required for matching external dependency headers.')
        if args.command == 'build':
            require(args.output is not None and not args.output.exists()
                    and not args.output.with_name(args.output.name + '.symbols').exists(), '--output must be new.')
        with workspace(args.work) as work:
            if args.command == 'fetch':
                fetch(work, lock)
                return
            src, build, gn, ninja, env, record = prepare(work, lock, args.platform, args.python,
                                                       args.engine.resolve(), args.build_name)
            run([gn, 'gen', build, '--fail-on-unused-args'], cwd=src, env=env, log=build / 'configure.log')
            print(f'Configured {args.platform}: {build}', flush=True)
            if args.command == 'configure':
                return
            run([ninja, '-C', build, '-j', args.jobs, 'webrtc'], cwd=src, env=env, log=build / 'compile.log')
            link_probe(work, lock, args.platform, build, env, record, args.engine.resolve())
            print(json.dumps(export_artifact(work, lock, args.platform, build, gn, env, record, args.output.resolve()), indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
