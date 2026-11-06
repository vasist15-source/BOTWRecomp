"""Game-free checks of Cemu's patch format (tools/cemu_patches.py) and the code mods built into the game
code (tools/botw_code_mods.py), on a small made-up game: python3 tools/test_cemu_patches.py"""
import json
import os
import re
import struct
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "recomp")]
import botw_code_mods as bcm  # noqa: E402
import cemu_patches as cp  # noqa: E402


def value(text, names=None, integer=False):
    names = names or {}
    return cp.evaluate(cp.compile_expression(text), lambda n: names[n], integer=integer)


def word(text, address=0x02000000):
    _, a = cp.assemble(text, address)
    assert not a.relocs, text
    return struct.unpack(">I", bytes(a.data))[0]


class Expressions(unittest.TestCase):
    def test_cemu_arithmetic(self):
        self.assertEqual(value("1 + 2 * 3"), 7)
        self.assertEqual(value("2 ^ 3 ^ 2"), 512)  # right-associative
        self.assertEqual(value("(1 + 2) * 3"), 9)
        self.assertEqual(value("10 / 0"), 0)  # division by zero is 0
        self.assertEqual(value("7.5 % 2"), 0)  # both rounded: 8 % 2
        self.assertEqual(value("0 - 7 % 3"), -1)
        self.assertEqual(value("-7 % 3"), -1)  # remainder takes the dividend's sign
        self.assertEqual(value("1 == 1.00000000001"), 1)
        self.assertEqual(value("3 > 2"), 1)
        self.assertEqual(value("2 <= 1"), 0)
        self.assertEqual(value("$x * -1", {"$x": 2.0}), -2)
        self.assertAlmostEqual(value("$c + (($c == 0) * $t)", {"$c": 0.0, "$t": 96.0}), 96)

    def test_numbers(self):
        self.assertEqual(value("0x10"), 16)
        self.assertEqual(value("010"), 16)          # a leading 0 makes hex ...
        self.assertEqual(value("0x0270f"), 9999)
        self.assertEqual(value("010 + 0.5"), 10.5)  # ... unless a decimal point follows anywhere
        self.assertEqual(value("1.5e2"), 150)

    def test_modifiers_apply_to_the_whole_value(self):
        names = {"a": float(0x12348000), "b": 4.0}
        self.assertEqual(value("a@ha", names), 0x1235)
        self.assertEqual(value("a@l", names), 0x8000)
        self.assertEqual(value("a@h", names), 0x1234)
        self.assertEqual(value("(a@ha + b@ha)", names), 0x1235)  # ha(a + b), as Cemu 2.7+
        with self.assertRaises(cp.PatchError):
            value("a@ha + b@l", names)
        self.assertEqual(value("ha(a) + 0x10000", names), 0x1236)  # Cemu applies a function to the rest

    def test_errors(self):
        for bad in ("-$x", "1 +", "(1", "1)", "1 $ 2"):
            with self.assertRaises((cp.PatchError, KeyError), msg=bad):
                value(bad, {"$x": 1.0})

    def test_integer_conditions(self):
        self.assertEqual(value("7 / 2", integer=True), 3)
        self.assertEqual(value("0 - 7 / 2", integer=True), -3)
        self.assertEqual(value("$m == 1", {"$m": 1.9}, integer=True), 1)

    def test_to_u32(self):
        self.assertEqual(cp.to_u32(-1.0), 0xFFFFFFFF)
        self.assertEqual(cp.to_u32(4294967296.0), 0)
        self.assertEqual(cp.to_u32(float("inf")), 0)


RULES = """[Definition]
titleIds = 00050000101C9300,00050000101C9400,00050000101C9500
name = Day Time
description = One|Two
version = 6

[Default]
$mode:int = 0
$mult = 1.0
$third = (1/3)
$twice = 0

[Preset]
name = Enabled
category = Cycle
default = 1
$mode:int = 1

[Preset]
name = Disabled
category = Cycle
$mode:int = 0

[Preset]
name = Fast
category = Length
condition = ($mode == 1)
$mult = 96
$twice = 2 * $mult

[Preset]
name = Normal
category = Length
condition = ($mode == 1)
default = 1
"""


class Rules(unittest.TestCase):
    def test_presets(self):
        r = cp.parse_rules(RULES)
        self.assertEqual(r.name, "Day Time")
        self.assertEqual(r.description, "One\nTwo")
        self.assertEqual(r.defaults["$mode"], ("int", 0.0))
        self.assertAlmostEqual(r.defaults["$third"][1], 1 / 3)
        self.assertEqual(r.categories, ["Cycle", "Length"])
        self.assertEqual([p.name for p in r.presets], ["Enabled", "Disabled", "Fast", "Normal"])
        self.assertTrue(r.presets[0].default and r.presets[3].default)
        self.assertEqual(r.presets[2].values, {"$mult": 96.0, "$twice": 192.0})  # same section's variables
        self.assertEqual(cp.rpn_to_json(r.presets[2].condition), ["$mode", 1.0, "=="])
        self.assertEqual(r.possible_values("$mode"), [0.0, 1.0])

    def test_preset_variable_must_be_declared(self):
        with self.assertRaises(cp.PatchError):
            cp.parse_rules(RULES.replace("$twice = 2 * $mult", "$other = 2"))


class Assembler(unittest.TestCase):
    VECTORS = [  # the game's own words (from botw_fps_patches / botw_cheat_patches) and well-known ones
        ("blr", 0x4E800020), ("nop", 0x60000000), ("mflr r0", 0x7C0802A6), ("mtlr r0", 0x7C0803A6),
        ("stwu r1, -0x28(r1)", 0x9421FFD8), ("li r3, 1", 0x38600001), ("cmpwi r3, 0", 0x2C030000),
        ("bctrl", 0x4E800421), ("mtctr r12", 0x7D8903A6), ("fmr f1, f31", 0xFC20F890),
        ("fmuls f0, f13, f12", 0xEC0D0332), ("fadds f29, f1, f30", 0xEFA1F02A), ("subf r0, r24, r0", 0x7C180050),
        ("addi r5, r5, -1", 0x38A5FFFF), ("addic. r11, r11, -1", 0x356BFFFF), ("stw r0, 0x16FC(r29)", 0x901D16FC),
        ("fsel f12, f12, f12, f31", 0xFD8CFB2E), ("mr r29, r11", 0x7D7D5B78), ("cmplw r0, r26", 0x7C00D040),
        ("stb r26, 0x18(r30)", 0x9B5E0018), ("lfs f11, 0x7964(r10)", 0xC16A7964), ("fcmpu f13, f29", 0xFC0DE800),
        ("fcmpu cr0, f13, f29", 0xFC0DE800), ("blt .-0x84", 0x4180FF7C), ("beq .+0x34", 0x41820034),
        ("fmuls f30, f30, f1", 0xEFDE0072), ("beqlr", 0x4D820020), ("bnelr cr7", 0x4C9E0020),
        ("beq cr1, .+8", 0x41860008), ("crxor 4*cr1+eq, 4*cr1+eq, 4*cr1+eq", 0x4CC63182),
        ("lbzx r0, r4, r5", 0x7C0428AE), ("slwi r3, r4, 2", 0x5483103A), ("srwi r3, r4, 8", 0x5483C23E),
        ("lis r9, 0x8B60", 0x3D208B60), ("subi r3, r3, 1", 0x3863FFFF), ("fctiwz f0, f1", 0xFC00081E),
        ("divwu r3, r4, r5", 0x7C642B96), ("add. r3, r4, r5", 0x7C642A15), ("oris r8, r8, 0xD15A", 0x6508D15A),
        ("bdnz .-8", 0x4200FFF8), ("cmpw cr7, r3, r4", 0x7F832000), ("bl .+8", 0x48000009), ("b .-4", 0x4BFFFFFC),
        ("cmpwi cr7, r3, -1", 0x2F83FFFF), ("bnl .+8", 0x41810008),  # Cemu reads bnl as bgt
    ]

    def test_vectors(self):
        for text, expected in self.VECTORS:
            with self.subTest(text):
                self.assertEqual(word(text), expected)

    def test_data(self):
        at, a = cp.assemble('.string "a\\tb\\x41"', 0x10)
        self.assertEqual(bytes(a.data), b"a\tbA\0")  # Cemu stops a string at a \x escape
        at, a = cp.assemble(".float 1.5", 0x11)
        self.assertEqual(at, 0x14)  # aligned to the element
        self.assertEqual(a.relocs[0].kind, "float")
        at, a = cp.assemble(".align 8", 0x13)
        self.assertEqual((at, len(a.data)), (0x13, 5))
        at, a = cp.assemble(".float 1.5", 0x11, fixed_address=True)
        self.assertEqual(at, 0x11)  # an <address> = line is never padded

    def test_errors(self):
        for bad in ("li r3", "lwz r3, r4", "addi r33, r3, 1", "frobnicate r3", "li r3, 0x10000"):
            with self.subTest(bad), self.assertRaises(cp.PatchError):
                cp.assemble(bad, 0)


PATCH = """[Common]
moduleMatches = 0x11111111, 0x6267BFD0
.origin = codecave
shared:
lis r3, value@ha
blr

[Test_V208]
moduleMatches = 0x6267BFD0
.origin = codecave
name:
.string "hi"
value:
.float $mult * 2
count = 3 + 4
dynamic = $mode + 1
helper:
li r4, dynamic
bl import.coreinit.OSReport
bla shared
blr
0x02000008 = bla helper
0x0200000C = nop
0x10000004 = .float $third
0x10000010 = fixed:
0x02000010 = lfs f1, fixed@l(r3)

[Other]
moduleMatches = 0x22222222
0x02000000 = nop
"""


def resolved(text=PATCH, rules=RULES, imports=None):
    groups = [g for g in cp.parse_patch_file(text, "patch_test.asm") if g.matches(0x6267BFD0)]
    nxt = [0x01800000]

    def cave(size):
        b = nxt[0]
        nxt[0] = (b + size + 255) & ~255
        return b
    return cp.resolve(groups, cp.parse_rules(rules), cave,
                      lambda a: 0x02000000 <= a < 0x02000100 or 0x10000000 <= a < 0x10000100, imports)


class Packs(unittest.TestCase):
    def test_resolve(self):
        r = resolved()
        self.assertEqual([c[:2] for c in r.caves], [(0x01800000, 8), (0x01800100, 24)])
        self.assertEqual(r.labels["name"], 0x01800100)
        self.assertEqual(r.labels["value"], 0x01800104)  # after the string, aligned for the float
        self.assertEqual(r.labels["helper"], 0x01800108)
        self.assertEqual(r.symbols["count"], 7)
        self.assertIsInstance(r.symbols["dynamic"], cp.Rpn)
        items = {i.address: i for i in r.items}
        self.assertEqual(struct.unpack(">I", bytes(items[0x01800000].data))[0], 0x3C600180)  # lis r3, value@ha
        self.assertEqual(items[0x01800104].values[0][0].kind, "float")
        self.assertEqual(cp.rpn_to_json(items[0x01800104].values[0][1]), ["$mult", 2.0, "*"])
        li = items[0x01800108]
        self.assertEqual(cp.rpn_to_json(li.values[0][1]), ["$mode", 1.0, "+", "fn:u32"])
        self.assertEqual(items[0x0180010C].import_call, ("coreinit", "OSReport"))
        self.assertEqual(struct.unpack(">I", bytes(items[0x01800110].data))[0], 0x49800003)  # bla shared (0x01800000)
        self.assertEqual(struct.unpack(">I", bytes(items[0x02000008].data))[0], 0x4980010B)  # bla helper
        self.assertEqual(struct.unpack(">I", bytes(items[0x02000010].data))[0], 0xC0230010)
        self.assertNotIn(0x02000000, items)  # [Other] is for another module
        self.assertEqual(cp.evaluate_runtime(li.values[0][1], {"$mode": 1.0}), 2)

    def test_refusals(self):
        with self.assertRaises(cp.PatchError):  # an import the game doesn't have
            resolved(imports={("coreinit", "OSFatal")})
        with self.assertRaises(cp.PatchError):  # a branch target from presets
            resolved(PATCH.replace("bla shared", "bla $mode"))
        with self.assertRaises(cp.PatchError):  # an unknown preset variable
            resolved(PATCH.replace("$mult * 2", "$nope"))
        with self.assertRaises(cp.PatchError):  # outside the code cave and the module
            resolved(PATCH.replace("0x0200000C = nop", "0x0300000C = nop"))


# ---- the BotW layer on a made-up game

def made_up_game():
    """A recompiler-like object: functions at 0x02000000 (8 instructions) and 0x02000020 (4), data at
    0x10000000, coreinit.OSReport imported."""
    words = [0x38600000, 0x38800001, 0x7C632214, 0x2C030000, 0x41820008, 0x38600002, 0xC0230010, 0x4E800020,
             0x7C0802A6, 0x4BFFFFE1, 0x7C0803A6, 0x4E800020]
    sections = [types.SimpleNamespace(addr=0x02000000, size=4 * len(words)), types.SimpleNamespace(addr=0x10000000, size=0x100)]
    p = types.SimpleNamespace(text_lo=0x02000000, text_hi=0x02000000 + 4 * len(words), words=tuple(words),
                              rpx=types.SimpleNamespace(sections=sections), jump_tables={})
    p.word = lambda a: p.words[(a - p.text_lo) >> 2]
    r = types.SimpleNamespace(p=p, entries={0x02000000, 0x02000020}, imm_override={},
                              imports={0xC0000000: ("coreinit.rpl", "OSReport", "f")})
    r.func_of = lambda a: 0x02000020 if a >= 0x02000020 else 0x02000000
    r.func_end = lambda s: 0x02000020 if s == 0x02000000 else p.text_hi
    r.sym = lambda a: a
    r.imp_name = lambda slot: "imp_coreinit_OSReport"
    return r


def function_text(r, start, end):
    import ppc2c

    class Ctx:
        imm_override = {}

        def branch(self, addr, tgt):
            return "goto L_%08X;" % tgt if start <= tgt < end else "MUSTTAIL return f_%08X(c);" % tgt

        def call(self, addr, tgt):
            return "f_%08X(c);" % tgt

        def ret(self):
            return "return;"

        def indirect_jump(self, addr):
            return "c->pc = c->ctr; MUSTTAIL return ppc_dispatch(c);"
    lines = ["void f_%08X(Cpu* __restrict c) {" % start]
    for a in range(start, end, 4):
        lines.append("    %s /* %08X: %08X */" % (ppc2c.translate(a, r.p.word(a), Ctx()), a, r.p.word(a)))
    return "\n".join(lines + ["}"])


GAME_PATCH = """[Game_V208]
moduleMatches = 0x6267BFD0
.origin = codecave
scale:
.float $mult
helper:
lis r12, scale@ha
lfs f1, scale@l(r12)
li r5, $mode
cmpwi r5, 1
beq skip
bl import.coreinit.OSReport
skip:
blr
0x02000004 = bla helper
0x0200000C = nop
0x02000014 = li r3, $mode
0x10000008 = .float $mult * 2
"""


class GameMods(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="botw-code-mods-")
        self.addCleanup(self.temp.cleanup)
        self.mods = os.path.join(self.temp.name, "Mods")
        os.makedirs(self.mods)

    def pack(self, name, patch=GAME_PATCH, wrap=False):
        folder = os.path.join(self.mods, name, "inner" if wrap else "")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "rules.txt"), "w") as f:
            f.write(RULES)
        with open(os.path.join(folder, "patch_game.asm"), "w") as f:
            f.write(patch)
        return folder

    def load(self, reserved=None):
        return bcm.CodeMods(made_up_game(), folder=self.mods, reserved=reserved, log=lambda *a: None)

    def test_built_in_behind_a_switch(self):
        self.pack("DayTime", wrap=True)
        cm = self.load()
        mod = cm.mods[0]
        self.assertEqual((mod.id, mod.problem, mod.switch), ("DayTime", "", 0))
        self.assertEqual(sorted(mod.sites), [0x02000004, 0x0200000C, 0x02000014])
        self.assertEqual(mod.entries, [0x01800004])
        r = cm.r
        out = cm.apply(function_text(r, 0x02000000, 0x02000020), 0x02000000)
        self.assertTrue(out.startswith('#include "botw_patches.h"\nvoid f_01800004(Cpu* __restrict c);\n'))
        self.assertIn("if (botw_patch_on(0)) { c->lr = 0x02000008u; f_01800004(c); } else { c->r[4] = 0u + 0x00000001u; }", out)
        self.assertIn("if (!botw_patch_on(0)) { cr_set_s(c, 0, (int32_t)c->r[3], 0); }", out)  # nop
        self.assertIn("if (botw_patch_on(0)) { c->r[3] = 0u + ((uint32_t)(int32_t)(int16_t)botw_patch_value(1)); }", out)
        for a in range(0x02000000, 0x02000020, 4):
            self.assertEqual(out.count("/* %08X: %08X */" % (a, r.p.word(a))), 1)
        self.assertEqual(cm.apply("unchanged", 0x02000020), "unchanged")
        cm.write(self.temp.name)
        with open(os.path.join(self.temp.name, "code_patches.c")) as f:
            c = f.read()
        self.assertIn("void f_01800004(Cpu* __restrict c) {", c)
        self.assertIn("imp_coreinit_OSReport(c);", c)
        self.assertIn("botw_patch_value(0)", c)
        self.assertIn("{0x01800004u, f_01800004}", c)
        text = "".join(re.findall(r'^    "(.*)"', c, re.M)).encode().decode("unicode_escape")
        manifest = json.loads(text)["mods"][0]
        self.assertEqual(manifest["fingerprint"], bcm.fingerprint(os.path.join(self.mods, "DayTime", "inner")))
        self.assertEqual(manifest["data"], [[0x10000008, "00000000"], [0x10000008, "float", ["$mult", 2.0, "*"]]])
        self.assertEqual(manifest["writes"], [[0x01800000, "float", ["$mult"]]])
        self.assertEqual(manifest["values"], [[0, ["$mode"]], [1, ["$mode"]]])
        self.assertEqual(manifest["memory"][0][0], 0x01800000)
        self.assertEqual([p["name"] for p in manifest["presets"]], ["Enabled", "Disabled", "Fast", "Normal"])

    def test_refused(self):
        self.pack("A")
        self.pack("B")  # the same instructions: the later one (by name) is refused
        self.pack("C", "[C]\nmoduleMatches = 0x6267BFD0\n.origin = codecave\njumper:\nb 0x02000028\n"
                       "0x02000024 = bla jumper\n")  # into the middle of the game's second function
        self.pack("D", "[D]\nmoduleMatches = 0x6267BFD0\n0x02000018 = .int $mode\n")
        cm = self.load()
        problems = {m.id: m.problem for m in cm.mods}
        self.assertEqual(problems["A"], "")
        self.assertIn("same game code as A", problems["B"])
        self.assertIn("not the start of a function", problems["C"])
        self.assertIn("computed from presets", problems["D"])
        self.assertEqual([m.switch for m in cm.mods], [0, -1, -1, -1])
        cm = bcm.CodeMods(made_up_game(), folder=self.mods, reserved={0x0200000C: "the port"}, log=lambda *a: None)
        self.assertIn("which the port already changes", cm.mods[0].problem)

    def test_nothing_without_packs(self):
        cm = self.load()
        text = function_text(cm.r, 0x02000000, 0x02000020)
        self.assertEqual(cm.apply(text, 0x02000000), text)
        self.assertEqual(cm.plain_functions(), set())
        cm.write(self.temp.name)
        with open(os.path.join(self.temp.name, "code_patches.c")) as f:
            c = f.read()
        self.assertIn("g_botw_patch_func_count = 0;", c)
        self.assertIn('"{\\"format\\":1,\\"mods\\":[]}"', c)

    def test_fingerprint_matches_the_runtime(self):
        # runtime/tools/code_patch_rules_test.cpp checks the same folder and value
        folder = os.path.join(self.temp.name, "fp")
        os.makedirs(folder)
        with open(os.path.join(folder, "rules.txt"), "w") as f:
            f.write("[Definition]\nname = T\n")
        with open(os.path.join(folder, "patch_A.asm"), "w") as f:
            f.write("[T]\nmoduleMatches = 0x6267BFD0\n0x02000000 = nop\n")
        with open(os.path.join(folder, "notes.txt"), "w") as f:
            f.write("not part of it\n")
        self.assertEqual(bcm.fingerprint(folder), "%s" % FINGERPRINT)

    def test_translate_value(self):
        import ppc2c
        ctx = bcm._Ctx(types.SimpleNamespace(), 0, 0, {})
        for text, c in (("addi r3, r4, 0", "c->r[3] = c->r[4] + ((uint32_t)(int32_t)(int16_t)V);"),
                        ("lis r3, 0", "c->r[3] = 0u + (((uint32_t)(uint16_t)V << 16));"),
                        ("ori r3, r3, 0", "c->r[3] = c->r[3] | ((uint32_t)(uint16_t)V);"),
                        ("cmpwi r3, 0", "cr_set_s(c, 0, (int32_t)c->r[3], ((int32_t)(int16_t)V));")):
            with self.subTest(text):
                self.assertEqual(bcm.translate_value(ppc2c.translate, 0x01800000, word(text), ctx, "V"), c)


FINGERPRINT = "c0e372459df36742f6ffec0ec1c7d12b2df0067f3072266c6ab7cc31620668b5"

if __name__ == "__main__":
    unittest.main()
