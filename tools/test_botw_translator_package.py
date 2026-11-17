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
        for name in ('botw_fps_patches.py', 'botw_cheat_patches.py', 'botw_aspect_patches.py', 'botw_code_mods.py', 'cemu_patches.py'):
            with self.subTest(name), mock.patch.object(setup, 'ROOT', str(self.root)):
                before = setup.recomp_stamp(str(self.rpx))
                patch = self.root / 'tools' / name
                patch.write_text(patch.read_text() + '\n# changed patch input\n')
                self.assertNotEqual(before, setup.recomp_stamp(str(self.rpx)))

    def test_code_mods_invalidate_translation_cache(self):
        mods = pathlib.Path(self.temp.name) / 'Mods'
        pack = mods / 'DayTime'
        pack.mkdir(parents=True)
        with mock.patch.object(setup, 'ROOT', str(self.root)), mock.patch.dict('os.environ', {'BOTW_MODS_DIR': str(mods)}):
            empty = setup.recomp_stamp(str(self.rpx))
            (pack / 'rules.txt').write_text('[Definition]\nname = Day Time\n')
            self.assertEqual(empty, setup.recomp_stamp(str(self.rpx)))  # no patch files: a content pack
            (pack / 'patch_DayTime.asm').write_text('[T]\nmoduleMatches = 0x6267BFD0\n0x02000000 = nop\n')
            with_mod = setup.recomp_stamp(str(self.rpx))
            self.assertNotEqual(empty, with_mod)
            (pack / 'patch_DayTime.asm').write_text('[T]\nmoduleMatches = 0x6267BFD0\n0x02000004 = nop\n')
            self.assertNotEqual(with_mod, setup.recomp_stamp(str(self.rpx)))
            with mock.patch.dict('os.environ', {'BOTW_CODE_MODS': '0'}):
                self.assertEqual(empty, setup.recomp_stamp(str(self.rpx)))

    def test_cheat_sites_keep_the_original_instruction(self):
        cheats = module('botw_cheat_patches', ROOT / 'tools/botw_cheat_patches.py')
        fake = {0x100: (0x7C180050, 'BOTW_CHEAT_ARROWS', cheats.NOP),
                0x104: (0x38600000, 'BOTW_CHEAT_AMIIBO', 0x38600001),
                0x108: (0xFC20F890, 'botw_cheat_unbreakable(1)', ('after', 'c->f[1].ps0 = 0.0;')),
                0x10C: (0x2C030000, 'BOTW_CHEAT_WEATHER', ('instead', 'c->r[3] = 0;'))}
        source = '\n'.join(['void f_00000100(Cpu* __restrict c) {',
                            '    c->r[0] = c->r[0] - c->r[24]; /* 00000100: 7C180050 */',
                            '    c->r[3] = 0u + 0x00000000u; /* 00000104: 38600000 */',
                            '    c->f[1].ps0 = c->f[31].ps0; /* 00000108: FC20F890 */',
                            '    cr_set_s(c, 0, (int32_t)c->r[3], 0); /* 0000010C: 2C030000 */', '}'])
        with mock.patch.dict(cheats.SITES, fake, clear=True):
            out = cheats.apply(source, list(fake), lambda address, word: f'NEW_{word:08X};')
            self.assertTrue(out.startswith('#include "botw_cheats.h"\n'))
            self.assertIn('if (!botw_cheat(BOTW_CHEAT_ARROWS)) { c->r[0] = c->r[0] - c->r[24]; }', out)
            self.assertIn('if (botw_cheat(BOTW_CHEAT_AMIIBO)) { NEW_38600001; } else { c->r[3] = 0u + 0x00000000u; }', out)
            self.assertIn('c->f[1].ps0 = c->f[31].ps0; if (botw_cheat_unbreakable(1)) { c->f[1].ps0 = 0.0; }', out)
            self.assertIn('if (botw_cheat(BOTW_CHEAT_WEATHER)) { c->r[3] = 0; } else { cr_set_s(c, 0, (int32_t)c->r[3], 0); }', out)
            for address, (word, _, _) in fake.items():
                self.assertEqual(out.count(f'/* {address:08X}: {word:08X} */'), 1)
            with self.assertRaises(ValueError):
                cheats.apply(source.replace('7C180050', '7C180051'), [0x100], lambda a, w: '')

    def test_aspect_sites_keep_the_original_instruction(self):
        aspect = module('botw_aspect_patches', ROOT / 'tools/botw_aspect_patches.py')
        source = '\n'.join(['void f_0386CF60(Cpu* __restrict c) {',
                            '    { double v = ppc_to_single(c, c->f[11].ps0 / c->f[12].ps0); c->f[12].ps0 = v; c->f[12].ps1 = v; } /* 0386D020: ED8B6024 */',
                            '    c->lr = 0x03C479FCu; imp_gx2_GX2SetVertexUniformReg(c); /* 03C479F8: 48700CE1 */',
                            '    { uint32_t ea = c->r[5] + 0xFFFF94D8u; { double v = ldf32(ea); c->f[11].ps0 = v; c->f[11].ps1 = v; } } /* 035B0A38: C16594D8 */',
                            '}'])
        out = aspect.apply(source, [0x0386D020, 0x03C479F8, 0x035B0A38])
        self.assertTrue(out.startswith('#include "botw_aspect.h"\n'))
        lines = out.splitlines()
        view = lines.index(next(l for l in lines if '/* 0386D020: ED8B6024 */' in l))
        self.assertIn('botw_aspect_view(c->f[12].ps0)', lines[view + 1])          # after the division
        call = lines.index(next(l for l in lines if '/* 03C479F8: 48700CE1 */' in l))
        self.assertIn('botw_aspect_tag_projection();', lines[call - 1])           # before the upload
        self.assertNotIn('ldf32(ea)', next(l for l in lines if '/* 035B0A38: C16594D8 */' in l))  # replaced
        self.assertIn('botw_aspect_grass()', out)
        for address, (word, _, _) in aspect.SITES.items():
            if address in (0x0386D020, 0x03C479F8, 0x035B0A38):
                self.assertEqual(out.count(f'/* {address:08X}: {word:08X} */'), 1)
        with self.assertRaises(ValueError):
            aspect.apply(source.replace('ED8B6024', 'ED8B6025'), [0x0386D020])

    def test_game_file_is_not_allowed_in_release(self):
        shutil.copyfile(self.rpx, self.root / 'U-King.rpx')
        with self.assertRaises(SystemExit):
            release.check_package(str(self.root))


if __name__ == '__main__':
    unittest.main()
