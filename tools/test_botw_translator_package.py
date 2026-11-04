"""Game-free checks of the installer's packaged translator and cache invalidation."""
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


release = module('botw_dmg', ROOT / 'tools/release/botw_dmg.py')
setup = module('botw_setup', ROOT / 'tools/installer/botw_setup.py')


class TranslatorPackage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='botw-translator-')
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name) / 'package'
        for rel in release.TRANSLATOR:
            dest = self.root / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, dest)
        shutil.copytree(ROOT / 'tools/recomp/builds', self.root / 'tools/recomp/builds')
        self.rpx = pathlib.Path(self.temp.name) / 'invalid.rpx'
        self.rpx.write_bytes(b'not a game executable')

    def test_packaged_import_and_unknown_program_rejection(self):
        release.check_package(str(self.root))
        # An isolated child can import only the shipped scripts, with no source checkout on sys.path.
        result = subprocess.run([sys.executable, '-I', '-B', '-c', '''
import sys
sys.path.insert(0, sys.argv[1])
import probe_recomp
try:
    probe_recomp.make_recompiler(sys.argv[2])
except ValueError as error:
    assert "verified v208" in str(error)
else:
    raise AssertionError("unknown program was accepted")
''', str(self.root / 'tools'), str(self.rpx)], cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_patch_module_invalidates_translation_cache(self):
        with mock.patch.object(setup, 'ROOT', str(self.root)):
            before = setup.recomp_stamp(str(self.rpx))
            patch = self.root / 'tools/botw_fps_patches.py'
            patch.write_text(patch.read_text() + '\n# changed patch input\n')
            self.assertNotEqual(before, setup.recomp_stamp(str(self.rpx)))

    def test_game_file_is_not_allowed_in_release(self):
        shutil.copyfile(self.rpx, self.root / 'U-King.rpx')
        with self.assertRaises(SystemExit):
            release.check_package(str(self.root))


if __name__ == '__main__':
    unittest.main()
