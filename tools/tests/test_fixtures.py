import copy
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'Fixtures/services'))
import fixture_stack
import media_fixtures
from provenance import source_hashes


class MediaFixtureTests(unittest.TestCase):
    def test_committed_hashes_and_generated_unreal_adapter(self):
        media_fixtures.check()
        manifest = json.loads((media_fixtures.FIXTURES / 'manifest.json').read_text())
        first = media_fixtures.artifacts()
        self.assertEqual(first, media_fixtures.artifacts())
        for name, data in first.items():
            self.assertEqual(hashlib.sha256(data).hexdigest(), manifest['files'][name]['sha256'])
            self.assertEqual(len(data), manifest['files'][name]['bytes'])

    def test_pcm_wave_duration_stereo_period_and_signal(self):
        files = media_fixtures.artifacts()
        with wave.open(io.BytesIO(files['tone.wav'])) as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()), (2, 2, 48000, 48000))
            self.assertEqual(wav.readframes(48000), files['tone.s16le'])
        frames = list(struct.iter_unpack('<hh', files['tone.s16le']))
        self.assertTrue(all(left == right for left, right in frames))
        self.assertEqual(frames[:1200] * 40, frames)
        self.assertEqual(max(left for left, _ in frames), 8000)
        # 11 cycles per canonical period: count positive-going crossings, including the first sample.
        values = [left for left, _ in frames[:1200]]
        self.assertEqual(sum(values[i - 1] < 0 <= value for i, value in enumerate(values)), 11)

    def test_video_quadrants_alpha_frame_changes_and_timestamp_contract(self):
        data = media_fixtures.artifacts()['quadrants.bgra']
        manifest = media_fixtures.manifest()['video']
        self.assertEqual([manifest['first_rtp_timestamp'] + n * manifest['rtp_timestamp_step'] for n in range(12)],
                         list(range(90000, 126000, 3000)))
        for frame in (0, 11):
            high = (160, 204)[frame == 11]
            for x, y, bgra in [(0, 0, (30, 30, high, 255)), (63, 0, (30, high, 30, 255)),
                                (0, 63, (high, 30, 30, 255)), (63, 63, (30, high, high, 255))]:
                offset = (frame * 4096 + y * 64 + x) * 4
                self.assertEqual(data[offset:offset + 4], bytes(bgra))

    def test_cli_materializes_verified_files_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            cmd = [sys.executable, str(ROOT / 'tools/media_fixtures.py'), '--output', directory]
            self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            for name, data in media_fixtures.artifacts().items():
                self.assertEqual((Path(directory) / name).read_bytes(), data)
            sentinel = Path(directory) / 'keep.txt'
            sentinel.write_text('preserve')
            self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            self.assertEqual(sentinel.read_text(), 'preserve')

    def test_signaling_fixture_inventory_is_versioned_and_hash_locked(self):
        root = ROOT / 'Fixtures/signaling/v1'
        manifest = json.loads((root / 'manifest.json').read_text())
        self.assertEqual(manifest['fixture_version'], 1)
        self.assertEqual(manifest['protocol_status'], 'draft-unimplemented')
        self.assertEqual(set(manifest['files']), {p.name for p in root.iterdir() if p.name != 'manifest.json'})
        for name, digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((root / name).read_bytes()).hexdigest(), digest)


class ServiceFixtureTests(unittest.TestCase):
    def test_external_credentials_permissions_compose_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {}, clear=True):
            runtime = Path(directory) / 'runtime'
            fixture_stack.initialize(runtime)
            compose = (runtime / 'compose.json').read_text()
            credentials = []
            for name in ('janus_api_secret', 'turn_shared_secret'):
                path = runtime / 'secrets' / name
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
                credentials.append(path.read_text())
                self.assertNotIn(credentials[-1].strip(), compose)
            self.assertNotEqual(*credentials)
            self.assertEqual(stat.S_IMODE(runtime.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((runtime / 'secrets').stat().st_mode), 0o700)
            with self.assertRaises(ValueError):
                fixture_stack.initialize(runtime)
            self.assertEqual((runtime / 'secrets/janus_api_secret').read_text(), credentials[0])

    def test_injected_credentials_validated_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory) / 'runtime'
            with patch.dict(os.environ, {'SIMURGHRTC_FIXTURE_TURN_SECRET': 'unsafe\nvalue'}):
                with self.assertRaises(ValueError):
                    fixture_stack.initialize(runtime)
                self.assertFalse(runtime.exists())
            with patch.dict(os.environ, {'SIMURGHRTC_FIXTURE_TURN_SECRET': 'a' * 32, 'SIMURGHRTC_FIXTURE_JANUS_SECRET': 'b' * 32}):
                fixture_stack.initialize(runtime)
                self.assertEqual((runtime / 'secrets/turn_shared_secret').read_text().strip(), 'a' * 32)

    def test_private_network_loopback_control_ports_and_no_turn_exposure(self):
        config = fixture_stack.compose_document()
        self.assertTrue(config['networks']['fixture']['internal'])
        self.assertNotIn('ports', config['services']['turn'])
        self.assertTrue(all(p.startswith('127.0.0.1:') for p in config['services']['janus']['ports']))
        for service in config['services'].values():
            self.assertTrue(service['read_only'])
            self.assertEqual(service['cap_drop'], ['ALL'])
            self.assertNotIn('environment', service)
            self.assertNotIn('network_mode', service)

    def test_build_uses_immutable_source_inputs(self):
        lock = json.loads(fixture_stack.LOCK.read_text())
        self.assertRegex(lock['base_image'], r'@sha256:[a-f0-9]{64}$')
        self.assertRegex(lock['debian_snapshot'], r'^\d{8}T\d{6}Z$')
        command = fixture_stack.build_command()
        for name in ('janus', 'coturn'):
            pin = lock[name]
            self.assertRegex(pin['commit'], r'^[a-f0-9]{40}$')
            self.assertRegex(pin['archive_sha256'], r'^[a-f0-9]{64}$')
            self.assertTrue(pin['archive_url'].endswith('/' + pin['commit']))
            self.assertIn(name.upper() + '_COMMIT=' + pin['commit'], command)
            self.assertIn(name.upper() + '_SHA256=' + pin['archive_sha256'], command)

    def test_evidence_requires_single_success_matching_sources_and_process(self):
        lock = json.loads(fixture_stack.LOCK.read_text())
        sources = source_hashes(fixture_stack.SERVICES)
        valid = {'passed': True, 'lock': lock, 'service_sources': sources}
        line = 'SIMURGHRTC_SERVICE_REPORT ' + json.dumps(valid) + '\n'
        check = lambda log, code=0: fixture_stack.validate_report(log, code, lock, sources)['passed']
        self.assertTrue(check(line))
        for output, code in [('', 0), (line + line, 0), ('SIMURGHRTC_SERVICE_REPORT {bad}\n', 0), (line, 1), (line, 124)]:
            self.assertFalse(check(output, code))
        stale = copy.deepcopy(valid)
        stale['service_sources']['Dockerfile'] = '0' * 64
        self.assertFalse(check('SIMURGHRTC_SERVICE_REPORT ' + json.dumps(stale)))
        stale = copy.deepcopy(valid)
        stale['lock']['janus']['tag'] = 'stale'
        self.assertFalse(check('SIMURGHRTC_SERVICE_REPORT ' + json.dumps(stale)))


if __name__ == '__main__':
    unittest.main()
