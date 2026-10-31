#!/usr/bin/env python3
"""Builds the BOTWRecomp release: BOTWRecomp-<version>.dmg with "BOTWRecomp Installer.app".

usage: tools/release/botw_dmg.py VERSION [--out DIR]

The installer app carries, in Contents/Resources/botwrecomp:
  tools/installer/botw_setup.py, the translator (tools/probe_recomp.py, tools/rpx.py, tools/recomp)
  kit/  the runtime, prebuilt for macOS 13+ (libbotw_runtime.a: the wwhd objects; cemu_latte, imgui),
        runtime/include for the generated code, and kit.json (compile flags, link line)
so a player needs only Apple's Command Line Tools: the installer translates their U-King.rpx, compiles
that with clang and links it to the kit.

No game code can get in: the kit is configured with a stub GEN_DIR (empty files), only the runtime's own
targets are built, and every file of the package is checked against an allow list before the DMG is made.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEPLOYMENT_TARGET = "13.0"
APP = "BOTWRecomp Installer.app"
TRANSLATOR = ["tools/probe_recomp.py", "tools/botw_fps_patches.py", "tools/rpx.py", "tools/recomp/analyze.py", "tools/recomp/builds.py",
              "tools/recomp/ppc2c.py", "tools/recomp/recomp.py"]


def sh(cmd, **kw):
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def build_kit(version, work):
    stub = os.path.join(work, "stubgen")
    os.makedirs(stub, exist_ok=True)
    for n in ("code_000.c", "table.c", "imports.c"):
        with open(os.path.join(stub, n), "w") as f:
            f.write("/* stub: the release kit never contains game code */\n")
    with open(os.path.join(stub, "funcs.h"), "w") as f:
        f.write("#pragma once\n")
    rt = os.path.join(work, "rt")
    sh(["cmake", "-S", ROOT, "-B", rt, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DWWHD_RENDERER=METAL",
        "-DWWHD_EXTRACTOR=OFF", "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + DEPLOYMENT_TARGET, "-DWWHD_VERSION=" + version,
        "-DGEN_DIR=" + stub])
    cmds = subprocess.run(["ninja", "-C", rt, "-t", "commands", "wwhd"], check=True, capture_output=True,
                          text=True).stdout.splitlines()
    link = cmds[-1].split(" && ")[1].split()  # ": && c++ ... && :"
    gen_cc = next(c for c in cmds if "code_000.c" in c).split()
    objs = [a for a in link if a.startswith("CMakeFiles/wwhd.dir/")]
    sh(["ninja", "-C", rt] + objs + ["cemu_latte", "imgui"])

    kit = os.path.join(work, "kit")
    shutil.rmtree(kit, ignore_errors=True)
    os.makedirs(os.path.join(kit, "lib"))
    lib = os.path.join(kit, "lib", "libbotw_runtime.a")
    sh(["/usr/bin/libtool", "-static", "-o", lib] + [os.path.join(rt, o) for o in objs], capture_output=True)
    for n in ("libcemu_latte.a", "libimgui.a"):
        shutil.copy2(os.path.join(rt, n), os.path.join(kit, "lib", n))
    shutil.copytree(os.path.join(ROOT, "runtime", "include"), os.path.join(kit, "include"))

    # compile flags of the generated code, without paths (the installer adds its own -I)
    cflags, skip = [], False
    for a in gen_cc[1:]:
        if skip:
            skip = False
            continue
        if a in ("-MT", "-MF", "-o", "-c", "-isysroot"):  # the player's clang finds its own SDK
            skip = True
            continue
        if a.startswith("-I") or a == "-MD":
            continue
        cflags.append(a)
    # the link line, with the kit's libraries and a placeholder for the player's objects
    out_link = []
    skip = False
    for a in link[1:]:
        if skip:
            skip = False
            continue
        if a == "-isysroot":  # CMake 4 names the SDK; the player's clang finds its own
            skip = True
            continue
        if a.startswith("CMakeFiles/wwhd.dir/"):
            if "-Wl,-force_load,@KIT@/lib/libbotw_runtime.a" not in out_link:
                out_link.append("-Wl,-force_load,@KIT@/lib/libbotw_runtime.a")  # keeps the HLE registrations
        elif a == "wwhd":
            out_link.append("@OUT@")
        elif a == "libgamecode.a":
            out_link.append("@GAMECODE@")
        elif a in ("libcemu_latte.a", "libimgui.a"):
            out_link.append("@KIT@/lib/" + a)
        elif a.endswith("/libz.tbd"):
            out_link.append("-lz")
        elif a.startswith("/") or a.startswith("-isysroot"):
            raise SystemExit("unexpected absolute path in the link line: " + a)
        else:
            out_link.append(a)
    files = sorted(os.path.relpath(os.path.join(d, n), kit) for d, _, ns in os.walk(kit) for n in ns)
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    with open(os.path.join(kit, "kit.json"), "w") as f:
        json.dump({"version": version, "commit": commit, "deployment_target": DEPLOYMENT_TARGET,
                   "cflags": cflags, "link": out_link, "files": files}, f, indent=1)
    print("kit: cflags %s" % " ".join(cflags))
    return kit


def check_package(res):
    """Allow list: our scripts, the kit, docs. Nothing else (in particular nothing from a dump)."""
    allowed = re.compile(r"^(tools/installer/botw_setup\.py|tools/probe_recomp\.py|tools/botw_fps_patches\.py|tools/rpx\.py|"
                         r"tools/recomp/(analyze|builds|ppc2c|recomp)\.py|tools/recomp/builds/[a-z]+\.json|"
                         r"kit/kit\.json|kit/lib/lib(botw_runtime|cemu_latte|imgui)\.a|kit/include/[a-z_]+\.h|"
                         r"LICENSE|README\.md)$")
    bad = [os.path.relpath(os.path.join(d, n), res) for d, _, ns in os.walk(res) for n in ns]
    bad = [p for p in bad if not allowed.match(p)]
    if bad:
        raise SystemExit("unexpected files in the package: %s" % ", ".join(bad))
    # the runtime library must not define any recompiled game function (f_XXXXXXXX); the only ones it has
    # are wwhd_unlinked.cpp's stubs (fatal errors for Wind Waker HD addresses BotW doesn't have)
    lines = subprocess.run(["nm", "-gjUA", os.path.join(res, "kit", "lib", "libbotw_runtime.a")],
                           capture_output=True, text=True).stdout.splitlines()
    game = [l for l in lines if re.search(r" _f_[0-9A-F]{8}$", l) and "wwhd_unlinked.cpp.o" not in l]
    if game:
        raise SystemExit("the runtime library defines game functions: %s" % ", ".join(game[:5]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    a = ap.parse_args()
    work = os.path.join(ROOT, "build", "kit")
    kit = build_kit(a.version, work)

    stage = os.path.join(a.out, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    sh([os.path.join(ROOT, "tools", "installer", "macos", "build.sh"), stage, "--release", a.version])
    res = os.path.join(stage, APP, "Contents", "Resources", "botwrecomp")
    for rel in TRANSLATOR + ["tools/installer/botw_setup.py", "LICENSE"]:
        os.makedirs(os.path.dirname(os.path.join(res, rel)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, rel), os.path.join(res, rel))
    shutil.copytree(os.path.join(ROOT, "tools", "recomp", "builds"), os.path.join(res, "tools", "recomp", "builds"))
    shutil.copy2(os.path.join(ROOT, "tools", "installer", "README.md"), os.path.join(res, "README.md"))
    shutil.copytree(kit, os.path.join(res, "kit"))
    check_package(res)
    sh(["codesign", "--force", "--deep", "-s", "-", os.path.join(stage, APP)])
    shutil.copy2(os.path.join(ROOT, "tools", "installer", "README.md"), os.path.join(stage, "Read Me.md"))
    os.symlink("/Applications", os.path.join(stage, "Applications"))

    dmg = os.path.join(a.out, "BOTWRecomp-%s.dmg" % a.version)
    if os.path.exists(dmg):
        os.remove(dmg)
    sh(["hdiutil", "create", "-volname", "BOTWRecomp %s" % a.version, "-srcfolder", stage, "-fs", "HFS+",
        "-format", "UDZO", "-imagekey", "zlib-level=9", dmg])
    print(dmg)


if __name__ == "__main__":
    sys.exit(main())
