"""Repeat acceptance must prove fresh outputs and unchanged deliverable bytes."""
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compare_native_builds import compare
import mobile_artifacts as a
from test_native_artifacts import make_native_payload, seal_native
from test_mobile_artifacts import archive, macho


class NativeRepeatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.roots = [Path(self.temp.name) / name for name in ('a', 'b', 'baseline')]
        for index, root in enumerate(self.roots):
            make_native_payload(root)
            build = a.read_json(root / 'Build/build.json')
            build.update(fresh_output=index < 2, build_directory='/work/out/' + root.name,
                         recipe_sha256='a'*64 if index < 2 else 'b'*64,
                         isolated_sha256=str(index)*64, archive_before_isolation_sha256=str(index)*64)
            (root / 'Build/build.json').write_text(json.dumps(build)); seal_native(root)

    def change(self, **fields):
        root = self.roots[1]; path = root / 'Build/build.json'
        build = a.read_json(path); build.update(fields); path.write_text(json.dumps(build)); seal_native(root)

    def test_same_payload_and_different_debug_records(self):
        report = compare(*self.roots)
        self.assertTrue(report['passed']); self.assertTrue(report['matches_accepted_archive'])
        self.assertFalse(report['independent_host_validated'])
        self.assertEqual(['Build/build.json'], report['record_differences_from_a']['b'])

    def test_existing_outputs_cannot_count_as_fresh(self):
        self.change(fresh_output=False)
        with self.assertRaisesRegex(ValueError, 'empty output'): compare(*self.roots)

    def test_same_output_directory_is_not_a_repeat(self):
        self.change(build_directory='/work/out/a')
        with self.assertRaisesRegex(ValueError, 'reused'): compare(*self.roots)

    def test_changed_compiler_or_recipe_is_rejected(self):
        self.change(recipe_sha256='c'*64)
        with self.assertRaisesRegex(ValueError, 'toolchain'): compare(*self.roots)

    def test_consistent_new_bytes_do_not_inherit_baseline_acceptance(self):
        for root in self.roots[:2]:
            (root / 'Lib/libwebrtc.a').write_bytes(archive(('isolated.o/', macho(platform=1) + b'changed')))
            seal_native(root)
        with self.assertRaisesRegex(ValueError, 'baseline payload mismatch'): compare(*self.roots)

    def test_changed_notice_inputs_are_rejected(self):
        root = self.roots[1]
        (root / 'Licenses/THIRD_PARTY_NOTICES.txt').write_text('different notice input'); seal_native(root)
        with self.assertRaisesRegex(ValueError, 'payload mismatch'): compare(*self.roots)

    def test_android_consumer_code_hash_may_vary_but_not_compiler_proof(self):
        for index, root in enumerate(self.roots):
            make_native_payload(root, 'Android')
            build = a.read_json(root / 'Build/build.json')
            build.update(fresh_output=index < 2, build_directory='/work/out/' + root.name, recipe_sha256='a'*64)
            (root / 'Build/build.json').write_text(json.dumps(build))
            path = root / 'Build/link-probe.json'; proof = a.read_json(path)
            proof['executable_sha256'] = str(index)*64; path.write_text(json.dumps(proof)); seal_native(root)
        self.assertTrue(compare(*self.roots)['passed'])
        path = self.roots[1] / 'Build/link-probe.json'; proof = a.read_json(path)
        proof['compiler'] += ' changed'; path.write_text(json.dumps(proof)); seal_native(self.roots[1])
        with self.assertRaisesRegex(ValueError, 'proof inputs'): compare(*self.roots)
