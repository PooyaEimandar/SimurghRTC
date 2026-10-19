"""Pinned Apple tools must not be truncated in place between repeat builds."""
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_native_candidate as candidate


class NativeToolInstallTests(unittest.TestCase):
    def test_repeat_install_preserves_bytes_and_uses_fresh_inodes(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / 'host').mkdir(); (work / 'downloads').mkdir()
            for name in ('gn', 'ninja'):
                with zipfile.ZipFile(work / 'host' / (name + '.zip'), 'w') as archive:
                    archive.writestr(name, ('pinned ' + name).encode())
            with tarfile.open(work / 'downloads/clang.tar.xz', 'w:xz') as archive:
                member = tarfile.TarInfo('bin/clang'); content = b'pinned compiler'
                member.size = len(content); member.mode = 0o755
                archive.addfile(member, io.BytesIO(content))
                link = tarfile.TarInfo('bin/clang++'); link.type = tarfile.SYMTYPE; link.linkname = 'clang'
                archive.addfile(link)
            lock = {'tools': {name: {} for name in ('gn', 'ninja', 'clang')}}
            with patch.object(candidate.mobile_build, 'download'):
                candidate.build_tools(work, lock)
                compiler = work / 'src/third_party/llvm-build/Release+Asserts/bin'
                paths = [work / 'host/gn', work / 'host/ninja', compiler / 'clang']
                inodes = [path.stat().st_ino for path in paths]
                contents = [path.read_bytes() for path in paths]
                candidate.build_tools(work, lock)
            self.assertEqual(contents, [path.read_bytes() for path in paths])
            self.assertTrue(all(path.stat().st_ino != inode for path, inode in zip(paths, inodes)))
            self.assertTrue(all(path.stat().st_mode & 0o111 for path in paths))
            self.assertTrue((compiler / 'clang++').is_symlink())
            self.assertEqual(b'pinned compiler', (compiler / 'clang++').read_bytes())
