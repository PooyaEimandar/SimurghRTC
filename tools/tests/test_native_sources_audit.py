"""Do not mistake build-only dependencies or incomplete graphs for shipped code."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from audit_native_sources import archive_objects, compile_sources, component


class NativeSourceAuditTests(unittest.TestCase):
    def test_implicit_build_tools_are_not_archive_members(self):
        self.assertEqual(['obj/a.o'], archive_objects(
            'build obj/simurgh_probe/libprobe.a: alink obj/a.o | clang_arm64/protoc || stamp\n'))

    def test_reject_nested_archives_missing_rules_and_duplicates(self):
        for text in ('', 'build obj/simurgh_probe/libprobe.a: alink obj/lib.a',
                     'build obj/simurgh_probe/libprobe.a: alink obj/a.o obj/a.o'):
            with self.assertRaises(ValueError):
                archive_objects(text)

    def test_only_selected_compile_rules_are_inventoried(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            (build / 'obj').mkdir()
            (build / 'obj/inputs.ninja').write_text(
                'build obj/a.o: cxx ../../a.cc | ../../header.h\n'
                'build obj/host.o: cxx ../../host.cc\n')
            self.assertEqual({'obj/a.o': (build / '../../a.cc').resolve()},
                             compile_sources(build, ['obj/a.o']))
            with self.assertRaisesRegex(ValueError, 'Missing compile'):
                compile_sources(build, ['obj/missing.o'])

    def test_android_split_debug_output_is_not_an_extra_source(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            (build / 'obj').mkdir()
            (build / 'obj/inputs.ninja').write_text(
                'build obj/a.o obj/a.dwo: cxx ../../a.cc | ../../header.h\n')
            self.assertEqual({'obj/a.o': (build / '../../a.cc').resolve()},
                             compile_sources(build, ['obj/a.o']))

    def test_embedded_components_are_not_hidden_under_webrtc(self):
        self.assertEqual('common_audio/third_party/ooura', component('common_audio/third_party/ooura/fft.c'))
        self.assertEqual('third_party/libjpeg_turbo', component('third_party/libjpeg_turbo/jdapimin.c'))
        self.assertEqual('webrtc', component('net/dcsctp/socket/dcsctp_socket.cc'))
