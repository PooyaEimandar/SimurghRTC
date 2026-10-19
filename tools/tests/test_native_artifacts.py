"""Contract and corruption cases for the isolated native artifact boundary."""
import copy
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import mobile_artifacts as a
import native_artifacts as native
from test_mobile_artifacts import archive, elf, macho

def seal_native(root, meta=None):
    if meta is None: meta = a.read_json(root / 'artifact.json')['metadata']
    (root / 'artifact.json').write_text(json.dumps({'metadata':meta,'files':{n:a.digest(p) for n,p in a.payload_files(root).items()}}))

def make_native_payload(root, platform='Mac'):
    pins = a.read_json(native.PINS)
    for name in native.REQUIRED:
        path = root / name; path.parent.mkdir(parents=True,exist_ok=True);path.write_text('Synthetic fixture; no runtime acceptance.')
    obj = elf() if platform == 'Android' else macho(platform={'Mac':1,'IOS':2,'IOSSimulator':7}[platform])
    (root / 'Lib/libwebrtc.a').write_bytes(archive(('isolated.o/',obj)))
    shutil.copy2(native.ROOT / ('Config/NativeWebRTC.android.lock.json' if platform == 'Android' else 'Config/NativeWebRTC.lock.json'),root / 'Build/source-lock.json')
    shutil.copy2(native.ROOT / 'Config/NativeBackend/simurgh_rtc_abi.h',root / 'Include/simurgh_rtc_abi.h')
    build = {'bridge_sources':pins['bridge_sources']}
    if platform == 'Android':
        build.update(ndk='27.2.12479018', compiler_runtime_sha256='a'*64)
        (root / 'Build/link-probe.json').write_text(json.dumps({'passed':True, 'minimum_api':26,
            'load_alignment':16384, 'compiler':'Synthetic clang version 18.0.3', 'compiler_runtime_sha256':'a'*64}))
    (root / 'Build/build.json').write_text(json.dumps(build))
    meta = {'schema':2,'artifact_id':'test-only-'+platform.lower(),'platform':platform,'architecture':'arm64','engine_version':'5.8.3',
            'adapter':'webrtc-m155-cabi-v1','configuration':'Release','dependencies':'isolated-boringssl-opus-vpx',
            'source_revision':pins['source_revision'],'abi_header_sha256':pins['abi_header_sha256']}
    seal_native(root,meta)

class NativeArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)/'artifact';make_native_payload(self.root)
    def test_each_target_and_no_inherited_runtime_acceptance(self):
        for platform in ('Mac','Android','IOS','IOSSimulator'):
            make_native_payload(self.root,platform);r=a.validate(self.root)
            self.assertEqual(platform,r['platform']);self.assertEqual(1,r['objects']);self.assertEqual('unverified',r['runtime_validation'])
    def test_resealed_header_or_source_lock_rejected(self):
        for name in ('Include/simurgh_rtc_abi.h','Build/source-lock.json'):
            make_native_payload(self.root);(self.root/name).write_text('changed');seal_native(self.root)
            with self.assertRaisesRegex(ValueError,'drift'): a.validate(self.root)
    def test_android_requires_ndk_link_and_matching_compiler_helpers(self):
        for key, value in [('passed',False),('minimum_api',35),('load_alignment',4096),('compiler_runtime_sha256','b'*64)]:
            make_native_payload(self.root,'Android')
            path=self.root/'Build/link-probe.json';proof=a.read_json(path);proof[key]=value
            path.write_text(json.dumps(proof));seal_native(self.root)
            with self.assertRaises(ValueError): a.validate(self.root)
        make_native_payload(self.root,'Android');(self.root/'Build/link-probe.json').unlink();seal_native(self.root)
        with self.assertRaisesRegex(ValueError,'NDK consumer'): a.validate(self.root)
    def test_resealed_bridge_change_rejected(self):
        build=a.read_json(self.root/'Build/build.json');build['bridge_sources']['runtime.cc']='0'*64
        (self.root/'Build/build.json').write_text(json.dumps(build));seal_native(self.root)
        with self.assertRaisesRegex(ValueError,'bridge source drift'): a.validate(self.root)
    def test_resealed_wrong_machine_or_apple_platform_rejected(self):
        for obj in (elf(),macho(platform=2),macho(platform=1,cpu=0x1000007)):
            (self.root/'Lib/libwebrtc.a').write_bytes(archive(('isolated.o/',obj)));seal_native(self.root)
            with self.assertRaises(ValueError): a.validate(self.root)
    def test_only_one_isolated_object_and_no_extra_binary(self):
        (self.root/'Lib/libwebrtc.a').write_bytes(archive(('a.o/',macho(platform=1)),('b.o/',macho(platform=1))));seal_native(self.root)
        with self.assertRaisesRegex(ValueError,'exactly one'): a.validate(self.root)
        make_native_payload(self.root);(self.root/'Lib/libcrypto.a').write_bytes(b'bad');seal_native(self.root)
        with self.assertRaisesRegex(ValueError,'Unexpected native binary'): a.validate(self.root)
    def test_retired_adapter_and_engine_dependencies_rejected(self):
        meta=a.read_json(self.root/'artifact.json')['metadata']
        for key,value in [('adapter','webrtc-5414-ue5.8.3-v1'),('dependencies','unreal-openssl-opus-vpx'),('source_revision','0'*40)]:
            bad=copy.deepcopy(meta);bad[key]=value;seal_native(self.root,bad)
            with self.assertRaisesRegex(ValueError,'Incompatible native'): a.validate(self.root)
    def test_dirty_and_missing_payloads_rejected(self):
        (self.root/'Lib/libwebrtc.a').write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError,'SHA256'): a.validate(self.root)
        make_native_payload(self.root);(self.root/'Include/simurgh_rtc_abi.h').unlink();seal_native(self.root)
        with self.assertRaisesRegex(ValueError,'inventory'): a.validate(self.root)
