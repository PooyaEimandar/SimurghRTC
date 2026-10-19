"""Validate the isolated M155 artifact format; older exports remain historical evidence."""
from pathlib import Path
import re
import mobile_artifacts as a
ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'Plugins/SimurghRTC/Source/SimurghRTCNative/NativeBackend.inputs.json'
REQUIRED = {'Lib/libwebrtc.a','Include/simurgh_rtc_abi.h','Build/source-lock.json','Build/args.gn','Build/build.json','Licenses/THIRD_PARTY_NOTICES.txt'}

def validate(root):
    document = a.read_json(root / 'artifact.json')
    a.require(set(document) == {'metadata','files'}, 'Expected metadata and files.')
    meta, inventory = document['metadata'], document['files']
    pins = a.read_json(PINS)
    for key, value in {'schema':2,'architecture':'arm64','engine_version':'5.8.3','adapter':'webrtc-m155-cabi-v1',
                       'configuration':'Release','dependencies':'isolated-boringssl-opus-vpx',
                       'source_revision':pins['source_revision'],'abi_header_sha256':pins['abi_header_sha256']}.items():
        a.require(meta.get(key) == value, 'Incompatible native ' + key)
    a.require(meta['platform'] in ('Mac','Android','IOS','IOSSimulator'), 'Unsupported native platform.')
    files = a.payload_files(root)
    a.require(set(inventory) == set(files) and REQUIRED <= set(files), 'Native artifact inventory differs from disk.')
    for name,path in files.items():
        a.require(a.digest(path) == inventory[name], 'SHA256 mismatch: ' + name)
        if path.suffix.lower() in ('.a','.o','.so','.dylib','.dll','.lib','.jar','.aar'):
            a.require(name == 'Lib/libwebrtc.a', 'Unexpected native binary: ' + name)
    for name in REQUIRED: a.require(files[name].stat().st_size > 0, 'Empty native input: ' + name)
    a.require(inventory['Include/simurgh_rtc_abi.h'] == pins['abi_header_sha256'], 'Native ABI header drift.')
    a.require(inventory['Build/source-lock.json'] == pins['android_source_lock_sha256' if meta['platform'] == 'Android' else 'source_lock_sha256'], 'Native source lock drift.')
    build = a.read_json(files['Build/build.json'])
    a.require(build['bridge_sources'] == pins['bridge_sources'], 'Native bridge source drift.')
    if meta['platform'] == 'Android':
        a.require('Build/link-probe.json' in files, 'Android artifact lacks its NDK consumer link check.')
        link = a.read_json(files['Build/link-probe.json'])
        a.require(link.get('passed') is True and type(link.get('minimum_api')) is int
                  and link['minimum_api'] == 26 and type(link.get('load_alignment')) is int
                  and link['load_alignment'] == 16384 and build.get('ndk') == '27.2.12479018'
                  and isinstance(link.get('compiler'), str)
                  and 'clang version 18.0.3' in link.get('compiler', ''), 'Invalid Android NDK consumer check.')
        runtime = link.get('compiler_runtime_sha256', '')
        a.require(isinstance(runtime, str) and re.fullmatch('[0-9a-f]{64}', runtime)
                  and runtime == build.get('compiler_runtime_sha256'), 'Android compiler runtime identity mismatch.')
    objects = a.validate_archive(files['Lib/libwebrtc.a'],meta['platform'])
    a.require(objects == 1, 'Expected exactly one isolated native object.')
    return {'artifact_id':meta['artifact_id'],'platform':meta['platform'],'architecture':'arm64','files':len(files),
            'objects':objects,'archive_bytes':files['Lib/libwebrtc.a'].stat().st_size,
            'manifest_sha256':a.digest(root / 'artifact.json'),'runtime_validation':'unverified'}

def refresh_pins():
    lock = a.read_json(ROOT / 'Config/NativeWebRTC.lock.json')
    config = ROOT / 'Config/NativeBackend'
    value = {'source_revision':lock['repositories']['src']['revision'],
             'source_lock_sha256':a.digest(ROOT / 'Config/NativeWebRTC.lock.json'),
             'android_source_lock_sha256':a.digest(ROOT / 'Config/NativeWebRTC.android.lock.json'),
             'abi_header_sha256':a.digest(config / 'simurgh_rtc_abi.h'),
             'bridge_sources':{p.name:a.digest(p) for p in config.iterdir() if p.is_file()}}
    import json
    PINS.write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')
if __name__ == '__main__': refresh_pins()
