"""Reject misleading probe evidence before accepting an Unreal Server run."""
import importlib.util
from pathlib import Path
import unittest
import tempfile

SPEC = importlib.util.spec_from_file_location('run_native_probe', Path(__file__).parents[1] / 'run_native_probe.py')
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def health(data_only=False):
    return {
        'status': 'ready', 'run_id': 'this-run', 'compiled_server_target': True,
        'dedicated_runtime': True, 'can_ever_render': False, 'factory_ready': True,
        'peer_connection_ready': True, 'rtc_session_connected': False,
        'factory_mode': 'data-only' if data_only else 'builtin-media',
        'dummy_audio_device': not data_only, 'audio_processing': not data_only,
        'engine_version': '5.8.3-0+UE5',
        'audio_codecs': [] if data_only else ['opus'],
        'video_codecs': [] if data_only else ['VP8'],
    }


class NativeProbeTests(unittest.TestCase):
    def test_requires_selected_artifact_and_backend_when_requested(self):
        report = dict(health(), artifact_manifest_sha256='a' * 64, webrtc_adapter=probe.BACKEND_VERSION)
        probe.validate_health(report, 'this-run', True, False, 'a' * 64)
        for field, value in [('artifact_manifest_sha256', 'b' * 64),
                             ('artifact_manifest_sha256', None), ('webrtc_adapter', '5414/M109')]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                probe.validate_health(dict(report, **{field: value}), 'this-run', True, False, 'a' * 64)

    def test_bind_failure_log_must_identify_native_artifact_and_server_mode(self):
        line = (f'[timestamp]LogSimurghRTCProbe: Display: SIMURGHRTC_PROBE_NATIVE backend={probe.BACKEND_VERSION} '
                f'artifact={"a" * 64} compiled_server=1 mode=builtin-media')
        probe.validate_native_log(line, 'a' * 64, True, False)
        for invalid in ['', line.replace('a' * 64, 'b' * 64), line.replace('compiled_server=1', 'compiled_server=0'),
                        line.replace('builtin-media', 'data-only'), line + '-unexpected']:
            with self.subTest(log=invalid), self.assertRaises(ValueError):
                probe.validate_native_log(invalid, 'a' * 64, True, False)

    def test_packaged_server_stdout_without_file_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / 'process.log').write_text('SIMURGHRTC_PROBE_STOPPED requests=1')
            name, text = probe.read_runtime_log(output)
            self.assertEqual('process.log', name)
            self.assertIn('SIMURGHRTC_PROBE_STOPPED', text)

    def test_accepts_media_and_data_only_factories(self):
        for data_only in (False, True):
            probe.validate_health(health(data_only), 'this-run', True, data_only)

    def test_rejects_stale_process_or_editor_as_server(self):
        for field, value in [('run_id', 'previous-run'), ('compiled_server_target', False),
                             ('engine_version', '5.8.0-0+UE5'), ('can_ever_render', True)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                probe.validate_health(dict(health(), **{field: value}), 'this-run', True, False)

    def test_rejects_incomplete_or_overstated_readiness(self):
        for field, value in [('factory_ready', False), ('peer_connection_ready', False),
                             ('rtc_session_connected', True), ('audio_processing', False),
                             ('audio_codecs', ['PCMU']), ('video_codecs', [])]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                probe.validate_health(dict(health(), **{field: value}), 'this-run', True, False)

    def test_data_only_rejects_media_dependencies(self):
        for field, value in [('audio_codecs', ['opus']), ('video_codecs', ['VP8']),
                             ('dummy_audio_device', True), ('audio_processing', True)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                probe.validate_health(dict(health(True), **{field: value}), 'this-run', True, True)


if __name__ == '__main__':
    unittest.main()
