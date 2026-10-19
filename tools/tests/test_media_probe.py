"""Evidence must distinguish codec, real RHI, mixer, and packaged-runtime success."""
import copy
import sys
import json
import tempfile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from run_media_probe import read_media_report, validate_report


class MediaEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(run_id='fixture', pid=123, engine_version='5.8.3-test', editor_build=False,
                           network_transport_tested=False, resources_released=True, passed=True, error='',
                           video_codec='VP8', video_encoder='libvpx', video_decoder='libvpx', audio_codec='opus',
                           encoded_frames=12, decoded_frames=12, codec_max_color_error=3, capture_max_color_error=0,
                           audio_packets=50, max_audio_packet_bytes=100, max_video_packet_bytes=300,
                           pcm_sample_rate=48000, pcm_channels=2, pcm_samples=96000, tone_correlation=0.999,
                           pcm_rms=5600, texture_uploads=2, render_target_captures=2, frame_bytes=16384,
                           max_in_flight_frames=1, pcm_queue_limit_bytes=192000, pcm_queued_bytes=192000,
                           pcm_consumed_bytes=192000, audio_callbacks=94, audio_callback_on_game_thread=False, rhi='Metal')

    def test_complete_packaged_evidence(self):
        validate_report(self.report, 'fixture', 123, False)

    def test_selected_backend_identity_and_mobile_report_kind(self):
        from run_mobile_probe import read_report
        identity = 'a' * 64
        with self.assertRaises(ValueError):
            validate_report(self.report, 'fixture', 123, False, artifact_sha256=identity)
        self.report.update(artifact_manifest_sha256=identity,
                           native_backend='M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d')
        validate_report(self.report, 'fixture', 123, False, artifact_sha256=identity)
        log = 'SIMURGHRTC_MEDIA_REPORT ' + json.dumps(self.report)
        self.assertIsNone(read_report(log, 'fixture'))
        self.assertEqual(read_report(log, 'fixture', 'SIMURGHRTC_MEDIA_REPORT '), self.report)
        self.report['native_backend'] = 'retired'
        with self.assertRaises(ValueError):
            validate_report(self.report, 'fixture', 123, False, artifact_sha256=identity)

    def test_packaged_stdout_fallback_requires_one_complete_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            log = 'LogSimurghRTCMediaProbe: Display: SIMURGHRTC_MEDIA_REPORT ' + json.dumps(self.report)
            report, source = read_media_report(output, log)
            self.assertEqual(source, 'runtime_log')
            validate_report(report, 'fixture', 123, False)
            for invalid in ['', log + '\n' + log, 'SIMURGHRTC_MEDIA_REPORT broken']:
                with self.assertRaises(ValueError):
                    read_media_report(output, invalid)

    def test_rejects_stale_editor_nullrhi_and_incomplete_playback(self):
        for key, value in [('run_id', 'stale'), ('pid', 321), ('editor_build', True), ('rhi', 'Null'),
                           ('pcm_consumed_bytes', 0), ('audio_callback_on_game_thread', True),
                           ('decoded_frames', 11), ('resources_released', False)]:
            with self.subTest(key=key):
                changed = copy.deepcopy(self.report)
                changed[key] = value
                with self.assertRaises(ValueError):
                    validate_report(changed, 'fixture', 123, False)

    def test_rejects_invalid_signal_and_color_metrics(self):
        for key, value in [('tone_correlation', float('nan')), ('pcm_rms', 0), ('capture_max_color_error', 100),
                           ('codec_max_color_error', -1), ('max_audio_packet_bytes', 5000)]:
            with self.subTest(key=key):
                changed = copy.deepcopy(self.report)
                changed[key] = value
                with self.assertRaises(ValueError):
                    validate_report(changed, 'fixture', 123, False)

    def test_negative_requires_specific_failure_not_arbitrary_crash(self):
        self.report.update(passed=False, error='Render-target probe requires a real RHI; NullRHI is not a capture pass.')
        validate_report(self.report, 'fixture', 123, False, 'null-rhi')
        self.report['error'] = 'Decoder unavailable'
        with self.assertRaises(ValueError):
            validate_report(self.report, 'fixture', 123, False, 'null-rhi')


if __name__ == '__main__':
    unittest.main()
