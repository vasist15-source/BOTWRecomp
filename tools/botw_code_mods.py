"""Cemu graphic packs' code patches (patch_*.asm) built into the translated BotW v208 game code.

The packs in the Mods folder (~/Library/Application Support/BotW/Mods, or BOTW_MODS_DIR; BOTW_CODE_MODS=0
leaves them out) whose patch files have groups for v208 (moduleMatches 0x6267BFD0) are read when the game
code is translated (tools/cemu_patches.py reads them as Cemu does) and built in, each behind a switch the
runtime sets (Settings > Mods, runtime/src/mods/code_patches.cpp):

- An instruction a pack changes keeps the game's own: the translated code asks the mod's switch
  (botw_patch_on, runtime/include/botw_patches.h) and runs the pack's instruction instead while it is on,
  as the cheats do (tools/botw_cheat_patches.py). Switched off, the game runs its own code.
- Code caves become functions of their own in code_patches.c (f_<address>), at 0x01800000 and up in guest
  memory (below the game's code, so a bla reaches them), one after another; the runtime writes the caves'
  bytes there at startup.
- What depends on a pack's presets the runtime computes from the chosen presets: cave data
  (.float $timeMultiplier), the game data a pack changes (written while the mod is on, the game's own put
  back when it is switched off) and instruction immediates (li r12, $dayTimeEnum: botw_patch_value).
  Choosing other presets needs no rebuild; changing a pack's files does (its fingerprint, below).
- A pack is refused, with the reason shown in Settings > Mods, where it changes game code the port changes
  itself (FPS++, the cheats, the frame-time fixes), where it changes the same game code as a mod before it
  (in name order), and where it needs what translated code can't do: jumps into the middle of the game's
  functions, a code word computed from presets, more memory ([RAM]), or code Cemu runs at startup.

code_patches.c always exists (with no packs it only holds empty tables, and every other file is the same
as without this module); g_botw_patch_manifest (JSON) tells the runtime which packs it holds, with a
SHA-256 of each pack's rules.txt and patch files.
"""
import hashlib
import json
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cemu_patches as cp  # noqa: E402

MODULE = 0x6267BFD0           # U-King.rpx v208, as Cemu's moduleMatches names it
CAVE_BASE = 0x01800000        # code caves: below the game's code (0x02000000), so bla can reach them
CAVE_END = 0x02000000
CAVE_ALIGN = 256              # as Cemu's RPLLoader_AllocateCodeCaveMem
MAX_MODS = 64                 # botw_patches.h BOTW_PATCH_MAX_MODS
MAX_VALUES = 1024             # botw_patches.h BOTW_PATCH_MAX_VALUES
SENTINELS = (0x1357, 0x2468)  # immediates that find a run-time value's place in translated C
FORMAT = 1                    # of the manifest
MAX_MANIFEST = 900 * 1024     # the runtime's JSON reader takes up to 1 MiB (mod_json.h)


def mods_dir():
    if os.environ.get("BOTW_MODS_DIR"):
        return os.environ["BOTW_MODS_DIR"]
    return os.path.join(os.path.expanduser("~"), "Library", "Application Support", "BotW", "Mods")


def hidden(name):
    return not name or name[0] == "." or name in ("__MACOSX", "Thumbs.db", "desktop.ini")


def pack_folder(path):
    """The folder with rules.txt in a mod folder, found as mods::loader::detect does (through folders
    around it, as unzipped), or None."""
    cur = path
    for _ in range(6):
        try:
            names = [n for n in os.listdir(cur) if not hidden(n)]
        except OSError:
            return None
        dirs = sorted(n for n in names if os.path.isdir(os.path.join(cur, n)))
        files = [n for n in names if not os.path.isdir(os.path.join(cur, n))]
        if any(n.lower() == "rules.txt" for n in files):
            return cur
        if any(n.lower() in ("content", "aoc", "romfs", "exefs") for n in dirs) or len(dirs) != 1:
            return None
        cur = os.path.join(cur, dirs[0])
    return None


def pack_files(pack):
    """rules.txt and the patch files Cemu reads, by their names on disk."""
    rules = [n for n in sorted(os.listdir(pack)) if n.lower() == "rules.txt"]
    patches, _ = cp.patch_files(pack)
    return rules[:1] + patches


def fingerprint(pack):
    """SHA-256 of a pack's rules.txt and patch files (name, size and bytes of each, in this order); the
    runtime computes the same (code_patches.cpp) to see whether the game code holds this version."""
    h = hashlib.sha256()
    for name in pack_files(pack):
        with open(os.path.join(pack, name), "rb") as f:
            data = f.read()
        h.update(name.encode("utf-8") + b"\0" + str(len(data)).encode() + b"\0" + data)
    return h.hexdigest()


def find_packs(folder):
    """[(id, pack folder)] of the Mods folder's packs with patch files, by name."""
    out = []
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        return out
    for name in names:
        path = os.path.join(folder, name)
        if hidden(name) or not os.path.isdir(path):
            continue
        pack = pack_folder(path)
        if pack and cp.patch_files(pack)[0]:
            out.append((name, pack))
    return out


def _word(data, at=0):
    return struct.unpack(">I", bytes(data[at:at + 4]))[0]


def _branch_target(addr, w):
    """(target, link) of a b/bc, or None."""
    op = w >> 26
    if op == 18:
        off = w & 0x03FFFFFC
        off = off - 0x04000000 if off & 0x02000000 else off
        return ((0 if w & 2 else addr) + off) & 0xFFFFFFFF, bool(w & 1)
    if op == 16:
        off = w & 0xFFFC
        off = off - 0x10000 if off & 0x8000 else off
        return ((0 if w & 2 else addr) + off) & 0xFFFFFFFF, bool(w & 1)
    return None


class Site:
    __slots__ = ("address", "word", "mod", "slot", "import_call", "function", "c", "labels", "calls")

    def __init__(self, address, word, mod, slot, import_call, function):
        self.address, self.word, self.mod, self.slot = address, word, mod, slot
        self.import_call, self.function = import_call, function
        self.c, self.labels, self.calls = None, set(), set()


class Mod:
    def __init__(self, mod_id, pack):
        self.id, self.pack = mod_id, pack
        self.name, self.description, self.fingerprint = mod_id, "", ""
        self.problem, self.notes = "", []
        self.switch = -1
        self.rules = None
        self.sites = {}      # address -> Site
        self.caves = []      # [base, size, memory (bytearray)]
        self.code = {}       # cave code word address -> (word, item)
        self.entries = []    # cave functions, sorted
        self.functions = {}  # cave entry -> C
        self.writes = []     # (address, kind, Rpn): cave data from presets
        self.data = []       # (address, bytes) or (address, kind, Rpn): game data while the mod is on
        self.values = []     # (slot, Rpn): instruction immediates from presets
        self.cave_slots = {}  # cave instruction address -> its value slot


class _Refuse(Exception):
    pass


class _Ctx:
    """ppc2c.translate's callbacks for a pack's instruction, in [start, end): a game function (a site) or
    a cave function."""

    def __init__(self, mods, start, end, imports_at):
        self.mods, self.start, self.end, self.imports_at = mods, start, end, imports_at
        self.imm_override = {}
        self.labels = set()
        self.calls = set()  # cave functions it calls or jumps to

    def _function(self, tgt, what):
        name = self.mods.function_name(tgt)
        if name is None:
            raise _Refuse("%s %08X, which is not the start of a function" % (what, tgt))
        if self.mods.is_cave_entry(tgt):
            self.calls.add(tgt)
        return name

    def branch(self, addr, tgt):
        if self.start <= tgt < self.end:
            self.labels.add(tgt)
            return ("PPC_LOOP(); " if tgt <= addr else "") + "goto L_%08X;" % tgt
        return "MUSTTAIL return %s(c);" % self._function(tgt, "jumps to")

    def call(self, addr, tgt):
        if addr in self.imports_at:
            return "%s(c);" % self.mods.import_name(*self.imports_at[addr])
        return "%s(c);" % self._function(tgt, "calls")

    def ret(self):
        return "return;"

    def indirect_jump(self, addr):
        return "c->pc = c->ctr; MUSTTAIL return ppc_dispatch(c);"


def _value_forms(s):
    """How ppc2c writes a 16-bit immediate s, and the C for a run-time one (V)."""
    return [("0x%08Xu" % s, "(uint32_t)(int32_t)(int16_t)V"), ("0x%08Xu" % (s << 16), "((uint32_t)(uint16_t)V << 16)"),
            ("0x%04Xu" % s, "(uint32_t)(uint16_t)V"), ("%d" % s, "(int32_t)(int16_t)V")]


def translate_value(translate, addr, word, ctx, value):
    """C for an instruction whose 16-bit immediate is the C expression `value` at run time: the
    instruction is translated with two sentinel immediates, and the C must differ exactly where ppc2c
    wrote the immediate."""
    outs = [translate(addr, (word & ~0xFFFF) | s, ctx) for s in SENTINELS]
    forms = [dict((t, k) for k, (t, _) in enumerate(_value_forms(s))) for s in SENTINELS]
    tokens = re.split(r"(0x[0-9A-F]+u|\b\d+\b)", outs[0])
    kinds = [forms[0].get(t) if i % 2 else None for i, t in enumerate(tokens)]
    if not any(k is not None for k in kinds):
        raise _Refuse("the instruction at %08X can't take a preset value" % addr)
    other = "".join(_value_forms(SENTINELS[1])[k][0] if k is not None else t for t, k in zip(tokens, kinds))
    if other != outs[1]:
        raise _Refuse("the instruction at %08X can't take a preset value" % addr)
    return "".join("(%s)" % _value_forms(0)[k][1].replace("V", value) if k is not None else t
                   for t, k in zip(tokens, kinds))


class CodeMods:
    """The code mods of one translation (see the module's doc). recompiler: probe_recomp's BotwRecompiler
    after its own changes; reserved: {address: what changes it}; reserved_functions: {start: what}."""

    def __init__(self, recompiler, folder=None, reserved=None, reserved_functions=None, log=print):
        import ppc2c
        self.translate = ppc2c.translate
        self.Unhandled = ppc2c.Unhandled
        self.r = recompiler
        self.folder = folder if folder is not None else mods_dir()
        self.reserved = reserved or {}
        self.reserved_functions = reserved_functions or {}
        self.log = log
        self.mods = []
        self.sites = {}          # address -> Site, of the mods built in
        self.site_functions = {}  # game function -> [Site]
        self.cave_entries = set()  # cave functions of the mods built in
        self.loading = None      # the mod being read
        self.slots = []          # (mod, Rpn)
        p = recompiler.p
        self.text_lo, self.text_hi = p.text_lo, p.text_hi
        self.sections = [(s.addr, s.addr + s.size) for s in p.rpx.sections if s.addr and s.size]
        self.jump_words = set()
        for base, count in p.jump_tables.values():
            self.jump_words.update(range(base, base + 4 * count, 4))
        self.imports = {}  # (module, function) -> import slot
        for slot, (lib, name, kind) in recompiler.imports.items():
            if kind == "f":
                self.imports[(lib[:-4] if lib.endswith(".rpl") else lib, name)] = slot
        if os.environ.get("BOTW_CODE_MODS") == "0":
            return
        cave_next = CAVE_BASE
        for mod_id, pack in find_packs(self.folder):
            mod = Mod(mod_id, pack)
            self.mods.append(mod)
            slots = len(self.slots)
            self.loading = mod
            try:
                if len([m for m in self.mods if not m.problem]) > MAX_MODS:
                    raise _Refuse("there are more than %d code mods" % MAX_MODS)
                end = self._load(mod, cave_next)
            except (cp.PatchError, _Refuse, OSError, UnicodeError) as e:
                self._refuse(mod, str(e), slots)
                continue
            finally:
                self.loading = None
            cave_next = end
            self.cave_entries.update(mod.entries)
            for site in mod.sites.values():
                self.sites[site.address] = site
                self.site_functions.setdefault(site.function, []).append(site)
        while len(json.dumps(self.manifest(), separators=(",", ":"))) > MAX_MANIFEST:  # the largest packs go
            big = max((m for m in self.mods if not m.problem), key=lambda m: sum(c[1] for c in m.caves) + len(m.data))
            for a in big.sites:
                del self.sites[a]
            for f in list(self.site_functions):
                self.site_functions[f] = [x for x in self.site_functions[f] if x.mod is not big]
                if not self.site_functions[f]:
                    del self.site_functions[f]
            self.cave_entries.difference_update(big.entries)
            self._refuse(big, "the code mods together are too large for the game code's list of them; leave this one out", len(self.slots))
        for i, mod in enumerate(m for m in self.mods if not m.problem):
            mod.switch = i
        for mod in self.mods:
            if mod.problem:
                self.log("code mods: %s is not built in: %s" % (mod.id, mod.problem))
            else:
                self.log("code mods: %s (%s): %d instructions in %d functions, %d bytes of code caves, %d preset values" % (
                    mod.id, mod.name, len(mod.sites), len(set(s.function for s in mod.sites.values())),
                    sum(c[1] for c in mod.caves), len(mod.values) + len(mod.writes)))

    # ---- reading a pack

    def _refuse(self, mod, why, slots):
        mod.problem = why
        mod.sites, mod.caves, mod.code, mod.entries, mod.functions = {}, [], {}, [], {}
        mod.writes, mod.data, mod.values, mod.cave_slots = [], [], [], {}
        del self.slots[slots:]

    def is_cave_entry(self, a):
        return a in self.cave_entries or (self.loading is not None and a in self.loading.entries)

    def _in_module(self, a):
        return any(lo <= a < hi for lo, hi in self.sections)

    def _load(self, mod, cave_next):
        mod.fingerprint = fingerprint(mod.pack)
        rules_name = [n for n in os.listdir(mod.pack) if n.lower() == "rules.txt"][0]
        rules = mod.rules = cp.parse_rules(cp.read_text(os.path.join(mod.pack, rules_name), 1 << 20))
        mod.name = rules.name or mod.id
        mod.description = rules.description
        if rules.title_ids and not set(rules.title_ids) & {"00050000101c9300", "00050000101c9400", "00050000101c9500"}:
            raise _Refuse("this graphic pack is for another game (its titleIds don't name Breath of the Wild)")
        if "ram" in rules.sections:
            raise _Refuse("it gives the game more memory ([RAM]), which this port can't")
        sizes = {"overwritewidth", "overwriteheight", "overwriteformat"}
        if rules.texture_keys & sizes:
            raise _Refuse("it changes the game's render sizes ([TextureRedefine]), which only graphics packs can")
        if rules.texture_keys - {"width", "height", "depth", "formats", "formatsexcluded", "tilemodes",
                                 "tilemodesexcluded"}:
            mod.notes.append("Its texture settings ([TextureRedefine]: %s) are not used." % ", ".join(
                sorted(rules.texture_keys - {"width", "height", "depth", "formats", "formatsexcluded", "tilemodes",
                                             "tilemodesexcluded"})))
        names = [n.lower() for n in os.listdir(mod.pack)]
        if any(n.endswith(("_ps.txt", "_vs.txt", "_gs.txt")) for n in names):
            raise _Refuse("it also replaces shaders: only its code patches would apply, and on their own they "
                          "can break the picture")
        groups = cp.load_groups(mod.pack, MODULE)
        if not groups:
            raise _Refuse("its code patches are for other versions of the game (none for v208, moduleMatches 0x6267BFD0)")
        nxt = [cave_next]

        def cave_base(size):
            base = nxt[0]
            nxt[0] = (base + size + CAVE_ALIGN - 1) & ~(CAVE_ALIGN - 1)
            if nxt[0] > CAVE_END:
                raise _Refuse("the code caves of all code mods are larger than %d MiB" % ((CAVE_END - CAVE_BASE) >> 20))
            return base
        resolved = cp.resolve(groups, rules, cave_base, self._in_module, set(self.imports))
        for base, size, _ in resolved.caves:
            mod.caves.append([base, size, bytearray(size)])

        def cave_of(a):
            for cave in mod.caves:
                if cave[0] <= a < cave[0] + cave[1]:
                    return cave
            return None
        imports_at = {}
        raw_sites = {}  # address -> (word, slot rpn, import, item)
        for item in resolved.items:
            a, data = item.address, item.data
            cave = cave_of(a)
            if cave is not None:
                if a + len(data) > cave[0] + cave[1]:
                    raise _Refuse("%s: past the end of its code cave" % item.entry.where())
                cave[2][a - cave[0]:a - cave[0] + len(data)] = data
                if item.code:
                    mod.code[a] = (_word(data), item)
                    if item.import_call:
                        imports_at[a] = item.import_call
                    for r, rpn in item.values:
                        mod.cave_slots[a] = self._value(mod, item, r, rpn)
                else:
                    for k in range(a & ~3, a + len(data), 4):  # data written over code
                        mod.code.pop(k, None)
                    for r, rpn in item.values:
                        mod.writes.append((a + r.at, r.kind, rpn))
                continue
            if self.text_lo <= a < self.text_hi:
                if a % 4 or len(data) % 4 or a + len(data) > self.text_hi:
                    raise _Refuse("%s: changes part of an instruction at %08X" % (item.entry.where(), a))
                if item.values and not item.code:
                    raise _Refuse("%s: the game code at %08X would be a word computed from presets" % (item.entry.where(), a))
                for k in range(0, len(data), 4):
                    rpn = None
                    if item.values:
                        r, rpn = item.values[0]
                        if len(item.values) > 1 or r.kind != "imm" or r.shift != 0 or r.bits != 16:
                            raise _Refuse("%s: only a 16-bit immediate can come from presets" % item.entry.where())
                    raw_sites[a + k] = (_word(data, k), rpn, item.import_call, item)
                    if item.import_call:
                        imports_at[a + k] = item.import_call
                continue
            # the game's data: written while the mod is on
            if 0x10327D60 <= a < 0x10327D70 and not any("FPS++" in n for n in mod.notes):
                mod.notes.append("It sets values that Cemu's FPS++ pack reads; the port's own FPS++ doesn't, so it "
                                 "may have no effect here.")
            if any(isinstance(d[1], (bytes, bytearray)) and d[0] == a for d in mod.data):
                mod.notes.append("%s: changes %08X again" % (item.entry.where(), a))
            mod.data.append((a, bytes(data)))
            for r, rpn in item.values:
                mod.data.append((a + r.at, r.kind, rpn))
        # cave functions: what sites and other cave code call or jump to, labels used as values, and
        # the targets of jumps from one cave function into the middle of another
        entries = set()
        for a, (w, rpn, imp, item) in raw_sites.items():
            t = _branch_target(a, w)
            if t and not imp and cave_of(t[0]) is not None:
                entries.add(t[0])
        for a, (w, item) in mod.code.items():
            t = _branch_target(a, w)
            if t and t[1] and a not in imports_at and cave_of(t[0]) is not None:
                entries.add(t[0])
        for item in resolved.items:
            for name in item.refs:
                v = resolved.labels.get(name)
                if v is not None and v in mod.code:
                    entries.add(v)
        while True:
            ordered = sorted(entries)
            new = set()
            for a, (w, item) in mod.code.items():
                t = _branch_target(a, w)
                if not t or t[1] or cave_of(t[0]) is None or t[0] in entries:
                    continue
                if self._cave_function(ordered, mod, t[0]) != self._cave_function(ordered, mod, a):
                    new.add(t[0])
            if not new:
                break
            entries |= new
        for e in entries:
            if e not in mod.code:
                raise _Refuse("calls or jumps to %08X in its code cave, which is data" % e)
        mod.entries = sorted(entries)
        # sites
        for a, (w, rpn, imp, item) in sorted(raw_sites.items()):
            func = self.r.func_of(a)
            if func is None:
                raise _Refuse("changes %08X, which is in no function of the game" % a)
            if a in self.reserved:
                raise _Refuse("changes the game code at %08X, which %s already changes" % (a, self.reserved[a]))
            if func in self.reserved_functions:
                raise _Refuse("changes the game code at %08X, in a function %s rewrites" % (a, self.reserved_functions[func]))
            if a in self.jump_words:
                raise _Refuse("changes the game's data at %08X (a jump table in its code)" % a)
            if a in self.r.imm_override:
                raise _Refuse("changes the instruction at %08X, which refers to a system library" % a)
            other = self.sites.get(a)
            if other is not None:
                raise _Refuse("changes the same game code as %s (%08X)" % (other.mod.id, a))
            if w == self.r.p.word(a) and rpn is None:
                continue  # the game's own instruction
            slot = self._value(mod, item, None, rpn) if rpn is not None else None
            mod.sites[a] = Site(a, w, mod, slot, imp, func)
        for site in mod.sites.values():
            self._translate_site(site, imports_at)
        for e in mod.entries:
            mod.functions[e] = self._emit_cave_function(mod, e, imports_at)
        self._check_presets(mod)
        return nxt[0]

    def _value(self, mod, item, r, rpn):
        """A slot of botw_patch_values for an instruction's immediate from presets."""
        if r is not None and (r.kind != "imm" or r.shift != 0 or r.bits != 16 or len(item.values) > 1):
            raise _Refuse("%s: only a 16-bit immediate can come from presets" % item.entry.where())
        if len(self.slots) >= MAX_VALUES:
            raise _Refuse("the code mods use more than %d preset values in instructions" % MAX_VALUES)
        slot = len(self.slots)
        self.slots.append((mod, rpn))
        mod.values.append((slot, rpn))
        return slot

    @staticmethod
    def _cave_function(ordered, mod, a):
        """The cave function a cave address belongs to (the last entry at or before it in its cave)."""
        best = None
        for e in ordered:
            if e > a:
                break
            best = e
        if best is None:
            return None
        for cave in mod.caves:
            if cave[0] <= a < cave[0] + cave[1]:
                return best if cave[0] <= best else None
        return None

    def function_name(self, tgt):
        if self.is_cave_entry(tgt):
            return "f_%08X" % tgt
        if self.text_lo <= tgt < self.text_hi and tgt in self.r.entries:
            return "f_%08X" % self.r.sym(tgt)
        return None

    def import_name(self, module, function):
        return self.r.imp_name(self.imports[(module, function)])

    def _translate_one(self, a, w, ctx, slot):
        try:
            if slot is not None:
                return translate_value(self.translate, a, w, ctx, "botw_patch_value(%d)" % slot)
            return self.translate(a, w, ctx)
        except self.Unhandled as e:
            raise _Refuse("the instruction %08X at %08X can't be translated (%s)" % (w, a, e))

    def _translate_site(self, site, imports_at):
        start = site.function
        ctx = _Ctx(self, start, self.r.func_end(start), imports_at)
        site.c = self._translate_one(site.address, site.word, ctx, site.slot)
        site.labels, site.calls = ctx.labels, ctx.calls

    def _emit_cave_function(self, mod, start, imports_at):
        cave = next(c for c in mod.caves if c[0] <= start < c[0] + c[1])
        later = [e for e in mod.entries if e > start]
        end = min(later[0] if later else cave[0] + cave[1], cave[0] + cave[1])
        ctx = _Ctx(self, start, end, imports_at)
        body = []
        in_data = False
        for a in range(start, end, 4):
            if a in mod.code:
                w, item = mod.code[a]
                body.append((a, w, self._translate_one(a, w, ctx, mod.cave_slots.get(a))))
                in_data = False
            elif not in_data:  # code that runs into its data stops there, visibly
                body.append((a, None, "ppc_unimplemented(c, 0x%08Xu, 0); /* the code cave's data */" % a))
                in_data = True
        for t in ctx.labels:
            if t not in mod.code:
                raise _Refuse("jumps to %08X in its code cave, which is data" % t)
        out = ["void f_%08X(Cpu* __restrict c) {" % start, "    PPC_ENTER(0x%08Xu);" % start]
        for a, w, s in body:
            if a in ctx.labels:
                out.append("L_%08X: ;" % a)
            out.append("    %s /* %08X: %s */" % (s, a, "%08X" % w if w is not None else "data"))
        if end in mod.code and end in mod.entries:
            out.append("    MUSTTAIL return f_%08X(c);" % end)
        else:
            out.append("    ppc_unimplemented(c, 0x%08Xu, 0); /* the end of the code cave */" % end)
        out.append("}")
        return "\n".join(out), ctx.calls

    def _check_presets(self, mod):
        """Every value the runtime computes must work out for the presets the pack has."""
        values = {k: v for k, (t, v) in mod.rules.defaults.items()}
        for p in mod.rules.presets:
            for k, v in p.values.items():
                values.setdefault(k, v)
        for k, v in values.items():
            if not math.isfinite(v):
                raise _Refuse("preset value %s is not a number" % k)
        programs = [rpn for _, rpn in mod.values] + [w[2] for w in mod.writes] + [d[2] for d in mod.data if len(d) == 3]
        for rpn in programs:
            for name in rpn.presets():
                if name not in values:
                    raise _Refuse("%s is not a variable of its rules.txt" % name)
            cp.evaluate_runtime(rpn, values)

    # ---- output

    def plain_functions(self):
        return set(self.site_functions)

    def apply(self, source, start):
        """Wraps the sites of game function `start` (in plain form) in their mods' switches."""
        sites = self.site_functions.get(start)
        if not sites:
            return source
        lines = source.splitlines()
        protos = set()
        for site in sorted(sites, key=lambda s: s.address):
            marker = "/* %08X: %08X */" % (site.address, self.r.p.word(site.address))
            matches = [i for i, line in enumerate(lines) if marker in line]
            if len(matches) != 1:
                raise ValueError("Missing or ambiguous code mod instruction %08X" % site.address)
            i = matches[0]
            original = lines[i][:lines[i].index(marker)].strip()
            on = "botw_patch_on(%d)" % site.mod.switch
            if site.word == 0x60000000 and site.slot is None:  # nop
                body = "if (!%s) { %s }" % (on, original)
            else:
                body = "if (%s) { %s } else { %s }" % (on, site.c, original)
            lines[i] = "    /* BOTW_PATCH_BEGIN %08X %s */\n    %s %s\n    /* BOTW_PATCH_END %08X */" % (
                site.address, site.mod.id.replace("*/", "* /"), body, marker, site.address)
            protos |= site.calls
        text = "\n".join(lines)
        labels = set()
        for site in sites:
            labels |= site.labels
        if labels:  # targets of the pack's branches that the game's own code doesn't jump to
            lines = text.splitlines()
            for t in sorted(labels):
                if "L_%08X: ;" % t in text:
                    continue
                marker = "/* %08X: " % t
                at = next((k for k, line in enumerate(lines) if marker in line), None)
                if at is None:
                    raise ValueError("No instruction %08X for a code mod's branch" % t)
                lines.insert(at, "L_%08X: ;" % t)
            text = "\n".join(lines)
        head = '#include "botw_patches.h"\n' + "".join("void f_%08X(Cpu* __restrict c);\n" % e for e in sorted(protos))
        return head + text

    def manifest(self):
        mods = []
        for mod in self.mods:
            m = {"id": mod.id, "name": mod.name, "description": mod.description, "fingerprint": mod.fingerprint}
            if mod.problem:
                m["problem"] = mod.problem
                mods.append(m)
                continue
            rules = mod.rules
            m["switch"] = mod.switch
            m["variables"] = {k: [t, v] for k, (t, v) in rules.defaults.items()}
            m["categories"] = rules.categories
            m["presets"] = [{"name": p.name, "category": p.category, "default": p.default,
                             "condition": cp.rpn_to_json(p.condition) if p.condition else None,
                             "values": p.values} for p in rules.presets]
            m["memory"] = [[c[0], bytes(c[2]).hex()] for c in mod.caves]
            m["writes"] = [[a, k, cp.rpn_to_json(rpn)] for a, k, rpn in mod.writes]
            data = []
            for d in mod.data:
                if len(d) == 2:
                    data.append([d[0], bytes(d[1]).hex()])
                else:
                    data.append([d[0], d[1], cp.rpn_to_json(d[2])])
            m["data"] = data
            m["values"] = [[slot, cp.rpn_to_json(rpn)] for slot, rpn in mod.values]
            m["sites"] = sorted(mod.sites)
            m["notes"] = mod.notes + rules.notes
            mods.append(m)
        return {"format": FORMAT, "mods": mods}

    def write(self, outdir):
        """code_patches.c: the cave functions, their table and the manifest."""
        out = ['/* Cemu graphic packs\' code patches built into this game code (tools/botw_code_mods.py). */',
               '#include "funcs.h"', '#include "botw_patches.h"', '']
        funcs = []
        for mod in self.mods:
            if mod.problem:
                continue
            for e in mod.entries:
                out.append("void f_%08X(Cpu* __restrict c);" % e)
        for mod in self.mods:
            if mod.problem:
                continue
            for e in mod.entries:
                text, _ = mod.functions[e]
                out += ["", "/* %s */" % mod.id.replace("*/", "* /"), text]
                funcs.append(e)
        out.append("")
        out.append("const BotwPatchFunc g_botw_patch_funcs[] = {%s};" % (
            ", ".join("{0x%08Xu, f_%08X}" % (e, e) for e in funcs) or "{0u, 0}"))
        out.append("const unsigned g_botw_patch_func_count = %d;" % len(funcs))
        text = json.dumps(self.manifest(), sort_keys=True, separators=(",", ":"), allow_nan=False)
        out.append("const char g_botw_patch_manifest[] =")
        for k in range(0, len(text), 2000):
            chunk = text[k:k + 2000].replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?")
            out.append('    "%s"' % chunk)
        out[-1] += ";"
        out.append("")
        path = os.path.join(outdir, "code_patches.c")
        data = "\n".join(out)
        try:
            with open(path, encoding="utf-8") as f:
                if f.read() == data:
                    return
        except OSError:
            pass
        with open(path, "w", encoding="utf-8") as f:
            f.write(data)
