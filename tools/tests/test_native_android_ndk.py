"""Fresh and reused native Android NDK caches must pass the same content checks."""
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from build_native_android import prepare_ndk
from mobile_artifacts import digest


class NativeAndroidNdkTests(unittest.TestCase):
    def test_fresh_extraction_has_receipt_and_reuse_checks_contents(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=root/'ndk.zip';install=root/'host/ndk'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('ndk/source.properties','Pkg.Revision = test')
            sha=digest(archive)
            prepare_ndk(archive,install,sha,extract=True)
            self.assertEqual((install/'.archive-sha256').read_text(),sha)
            prepare_ndk(archive,install,sha,extract=False)
            (install/'ndk/source.properties').write_text('changed')
            with self.assertRaisesRegex(ValueError,'drift'):prepare_ndk(archive,install,sha,extract=False)

    def test_wrong_or_missing_receipt_cannot_validate_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=root/'ndk.zip';install=root/'install'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('ndk/file','content')
            sha=digest(archive);prepare_ndk(archive,install,sha,extract=True)
            marker=install/'.archive-sha256';marker.write_text('wrong')
            with self.assertRaisesRegex(ValueError,'receipt'):prepare_ndk(archive,install,sha,extract=False)
            marker.unlink()
            with self.assertRaisesRegex(ValueError,'receipt'):prepare_ndk(archive,install,sha,extract=True)


if __name__=='__main__':unittest.main()
