#!/usr/bin/env python3
"""Write runtime/src/aspect_hud_rules.inc: which HUD and menu panes are scaled or moved at an aspect ratio
wider than 16:9, and how.

usage: tools/botw_hud_rules.py CEMU_BOTW_GRAPHICS_PACK_DIR [OUT]

The rules are those of Cemu graphic packs' Breath of the Wild Graphics pack (patch_GUIAspectRatio.asm with
the names of patch_GUIScreens.asm, v208; https://github.com/cemu-project/cemu_graphic_packs, CC0-1.0), its
"Edge HUD" and "Centered HUD" modes. The pack is PowerPC code that runs when the game builds a pane from a
layout: it looks at the screen being loaded, the last parts pane ("Pa...") seen and the pane's name, and
jumps to one of some forty small routines that change the pane's x scale, x position or width. This script
turns that decision code into a table the runtime walks (runtime/src/aspect_hud.h has the walker and the
routines, written out by hand from the pack's). The pack itself is not shipped and not read at run time:
run this again only to pick up a newer pack. Anything in the decision code this script doesn't know stops
it, so nothing is dropped silently.
"""
import os
import re
import sys


def clean(path):
    """[(text, line number)]: no comments, no blank lines, no section headers."""
    out = []
    for number, raw in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        text = raw.strip()
        if not text or text[0] in ";#[" or text.startswith(("moduleMatches", ".origin")):
            continue
        text = re.split(r"\s[;#]", text)[0].strip()
        if text:
            out.append((text, number))
    return out


def strings(lines):
    found = {}
    for (text, _), (after, _) in zip(lines, lines[1:]):
        m = re.fullmatch(r"(\w+):", text)
        s = re.fullmatch(r'\.string "(.*)"', after)
        if m and s:
            found[m.group(1)] = s.group(1)
    return found


def convert(pack):
    code = clean(os.path.join(pack, "patch_GUIAspectRatio.asm"))
    names = strings(clean(os.path.join(pack, "patch_GUIScreens.asm")))
    names.update(strings(code))
    texts = [t for t, _ in code]
    begin = texts.index("checkScreenNames:")
    routines = texts.index("scalePaneAndSize:")        # the first routine that changes a pane
    end = texts.index("exitScale:")
    labels_in = lambda lo, hi: {m.group(1) for t in texts[lo:hi] for m in [re.fullmatch(r"(\w+):", t)] if m}
    actions = (labels_in(routines, end) | {"exitScale", "scaleMapAndStore"}) - {"const_640", "const_1"}
    floats = {}
    for (text, _), (after, _) in zip(code, code[1:]):
        m = re.fullmatch(r"(const_\w+):", text)
        f = re.fullmatch(r"\.float (.*)", after)
        if m and f:
            floats[m.group(1)] = f.group(1)

    ops, labels, used = [], {}, set()
    body = code[begin:routines]

    def at(k):
        return body[k][0] if k < len(body) else ""

    k = 0
    while k < len(body):
        text, number = body[k]
        m = re.fullmatch(r"(\w+):", text)
        if m:
            if m.group(1).startswith("const_"):
                k += 2                                   # its .float
                continue
            labels[m.group(1)] = len(ops)
            k += 1
            continue
        if text == "addi r5, r31, 0x80":
            ops.append(("kPane", None, None, None))
            k += 1
            continue
        m = re.fullmatch(r"lis (r5|r10), (\w+)@ha", text)
        if m:
            reg, sym = m.groups()
            if reg == "r5" and at(k + 1) == "addi r5, r5, %s@l" % sym and sym in ("copySubPanelString", "copyScreenString"):
                ops.append(("kParts" if sym == "copySubPanelString" else "kScreen", None, None, None))
                k += 2
                continue
            if (reg == "r5" and sym == "copySubPanelString" and at(k + 1) == "lbz r5, copySubPanelString@l(r5)"
                    and at(k + 2) == "cmpwi r5, 0"):
                b = re.fullmatch(r"(beq|bne) (\w+)", at(k + 3))
                if b:
                    ops.append(("kIfNoParts" if b.group(1) == "beq" else "kIfParts", b.group(2), None, None))
                    k += 4
                    continue
            if reg == "r10" and at(k + 1) == "addi r10, r10, %s@l" % sym and at(k + 2) == "bla _compareString":
                b = re.fullmatch(r"(beq|bne) (\w+)", at(k + 3))
                if b and sym in names:
                    ops.append(("kIf" if b.group(1) == "beq" else "kIfNot", b.group(2), names[sym], None))
                    k += 4
                    continue
            if reg == "r10" and at(k + 1) == "lfs f12, %s@l(r10)" % sym and sym in floats:
                value = floats[sym]
                if sym == "const_TimeOffset":            # the one that depends on the HUD mode (aspect_hud.h)
                    ops.append(("kOffsetTime", None, None, None))
                else:
                    ops.append(("kOffset", None, None, float(value)))
                k += 2
                continue
        m = re.fullmatch(r"b (\w+)", text)
        if m:
            ops.append(("kGo", m.group(1), None, None))
            k += 1
            continue
        raise SystemExit("patch_GUIAspectRatio.asm:%d: not understood: %s" % (number, text))

    out = []
    for op, target, name, value in ops:
        if target is None:
            where = "0"
        elif target in labels:
            where = str(labels[target])
        elif target in actions:
            used.add(target)
            where = "kDo | a_%s" % target
        else:
            raise SystemExit("unknown target %s" % target)
        fields = [op, where]
        if name is not None:
            fields.append('"%s"' % name)
        elif value is not None:
            fields += ["nullptr", "%gf" % value if "." in "%g" % value else "%g.0f" % value]
        out.append("    {%s}," % ", ".join(fields))
    return out, sorted(used), labels["checkScreenNames"]


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    ops, actions, entry = convert(argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    target = argv[2] if len(argv) > 2 else os.path.join(here, "..", "runtime", "src", "aspect_hud_rules.inc")
    version = ""
    try:
        version = open(os.path.join(argv[1], "..", "..", "version.txt")).read().strip().split("\n")[0]
    except OSError:
        pass
    with open(target, "w") as f:
        f.write("// Generated by tools/botw_hud_rules.py from Cemu graphic packs' BotW Graphics pack\n"
                "// (patch_GUIAspectRatio.asm, patch_GUIScreens.asm; %s; CC0-1.0). Do not edit: see that script.\n"
                % (version or "cemu_graphic_packs"))
        f.write("enum Action : uint16_t {\n")
        for a in actions:
            f.write("    a_%s,\n" % a)
        f.write("};\n")
        f.write("constexpr uint16_t kEntry = %d;\n" % entry)
        f.write("constexpr Op kOps[] = {\n" + "\n".join(ops) + "\n};\n")
    print("%d steps, %d routines used -> %s" % (len(ops), len(actions), os.path.normpath(target)))


if __name__ == "__main__":
    main(sys.argv)
