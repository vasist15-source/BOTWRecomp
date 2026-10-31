#!/usr/bin/env python3
"""Breath of the Wild for Mac: build the game from the player's own Wii U dump.

usage:
  botw_setup.py scan DIR                 find the base game, update and DLC folders under DIR
  botw_setup.py check [--base B] [--update U] [--dlc D] [--save S]
  botw_setup.py requirements             Command Line Tools, CMake, disk space
  botw_setup.py install --base B --update U [--dlc D] [--save S] [--dest DIR]
                        [--copy-game] [--keep-build] [--jobs N]

--json switches the output to JSON (one object for scan/check/requirements, JSON lines with
progress events for install); the installer app (tools/installer/macos) reads that. Standard
library only, Python 3.9 (the Command Line Tools' /usr/bin/python3).

Nothing from the game leaves the player's Mac: the generated code and the build live in
~/Library/Caches/BotW/installer, the app is written where the player chose, and the app reads the
game, update, DLC and save from the paths in ~/Library/Application Support/BotW/install.plist
(wwhd itself, runtime/src/platform/app_launch.mm: a launcher script would keep macOS from seeing the
game's window, and so from starting Game Mode).
"""
import argparse
import hashlib
import json
import os
import platform
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOME = os.path.expanduser("~")
SUPPORT = os.path.join(HOME, "Library", "Application Support", "BotW")
CACHE = os.path.join(HOME, "Library", "Caches", "BotW", "installer")
LOGS = os.path.join(HOME, "Library", "Logs", "BotW")
CONFIG = os.path.join(SUPPORT, "install.plist")
# A release (the installer app's Resources/botwrecomp) has the runtime prebuilt in kit/ (tools/release/
# botw_dmg.py): only the game code is compiled here, with the Command Line Tools' clang, and linked to
# it. A source checkout has no kit/ and builds everything with CMake.
KIT = os.path.join(ROOT, "kit")
SAVE = os.path.join(SUPPORT, "save")
GAME_COPY = os.path.join(SUPPORT, "game")
APP_NAME = "Breath of the Wild.app"
BUNDLE_ID = "local.botw-mac"  # same as tools/make_app.sh: settings kept by bundle id are shared

# The Mac port is built for the USA release with update v208 (game version 1.5.0)
TITLE_LOW = "101C9400"
BASE_TID, UPDATE_TID, DLC_TID = "00050000" + TITLE_LOW, "0005000E" + TITLE_LOW, "0005000C" + TITLE_LOW
UPDATE_VERSION = 208
DLC_LATEST = 80  # DLC v80: The Master Trials and The Champions' Ballad
RPX_SHA256 = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"  # U-King.rpx of v208
OTHER_REGIONS = {"101C9500": "European", "101C9300": "Japanese"}
KINDS = {"00050000": "base", "0005000E": "update", "0005000C": "dlc"}
KIND_NAMES = {"base": "base game", "update": "update", "dlc": "DLC"}
GEN_WEIGHT = 4.0  # compile time of a generated code file relative to a runtime source file
FUNCS_PER_FILE = 30000  # as tools/probe_recomp.py; build/gen of the dev checkout uses the same

# rough durations on an M-series Mac with 10 cores, for the time estimate before a step reports progress
EST_RECOMP = 35.0
EST_COMPILE_CPU = 2600.0  # CPU seconds: the generated code ~2200, the runtime ~300, then the link


# ---------------------------------------------------------------- dump checks

def read_title(d):
    """(title id, title version) of an extracted title folder, from code/app.xml (meta/meta.xml as fallback)."""
    for rel, hexver in (("code/app.xml", True), ("meta/meta.xml", False)):
        p = os.path.join(d, rel)
        if not os.path.isfile(p):
            continue
        try:
            root = ET.parse(p).getroot()
        except (ET.ParseError, OSError):
            continue
        tid, ver = root.findtext("title_id"), root.findtext("title_version")
        if not tid:
            continue
        try:
            v = int(ver, 16 if hexver else 10) if ver else None
        except ValueError:
            v = None
        return tid.strip().upper(), v
    return None, None


def describe_not_a_folder(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".wud", ".wux", ".iso"):
        return "This is a disc image. Extract it first (in Cemu: File > Install game title, or a tool such as " \
               "WiiUDownloader / CDecrypt), then choose the extracted folder with code, content and meta in it."
    if ext == ".wua":
        return "This is a Cemu archive (.wua). Extract it first (Cemu: File > Install game title, or the " \
               "\"zarchive\" tool), then choose the folder with code, content and meta in it."
    if ext == ".rpx":
        return "Choose the whole title folder (the one with code, content and meta in it), not the .rpx file."
    return "Choose a folder, not a file."


def check_title(path, kind):
    """Checks one folder of the dump. Returns {status: ok|warn|error|empty, title, detail, messages, ...}."""
    r = {"kind": kind, "path": path or "", "status": "empty", "title": "", "detail": "", "messages": []}

    def fail(msg, title="Not usable"):
        r.update(status="error", title=title)
        r["messages"].append(msg)
        return r

    if not path:
        r["title"] = "Optional" if kind == "dlc" else "Not chosen yet"
        return r
    if not os.path.exists(path):
        return fail("The folder does not exist (anymore): %s" % path, "Not found")
    if not os.path.isdir(path):
        return fail(describe_not_a_folder(path))
    names = set(os.listdir(path))
    lower = {n.lower() for n in names}
    if "title.tmd" in lower or any(n.lower().endswith(".app") and re.match(r"^[0-9a-f]{8}\.app$", n.lower()) for n in names):
        return fail("These are encrypted download files (title.tmd, 0000000x.app). Decrypt them first, for example "
                    "by installing the title in Cemu (File > Install game title), then choose the installed folder "
                    "(mlc01/usr/title/...).", "Encrypted files")
    if not {"code", "content", "meta"} <= lower:
        missing = ", ".join(sorted({"code", "content", "meta"} - lower))
        return fail("A Wii U title folder has code, content and meta in it; this one has no %s. Choose the folder "
                    "that contains them." % missing, "Not a title folder")
    tid, ver = read_title(path)
    if not tid:
        return fail("code/app.xml is missing or unreadable, so the title can't be identified.", "Unknown title")
    r["title_id"], r["version"] = tid, ver
    hi, lo = tid[:8], tid[8:]
    found = KINDS.get(hi)
    if lo in OTHER_REGIONS:
        return fail("This is the %s release (title %s). The Mac port supports the USA release (%s) only for now."
                    % (OTHER_REGIONS[lo], tid, BASE_TID), "Unsupported region")
    if lo != TITLE_LOW:
        return fail("This is not Breath of the Wild (title %s)." % tid, "Different game")
    if found != kind:
        return fail("This is the %s, not the %s. Choose it in the %s row." % (
            KIND_NAMES.get(found, "title " + tid), KIND_NAMES[kind], KIND_NAMES.get(found, "right")), "Wrong folder")

    if kind == "base":
        need = ["code/U-King.rpx", "meta/meta.xml", "content/Pack/Bootup.pack"]
        missing = [n for n in need if not os.path.isfile(os.path.join(path, n))]
        if missing:
            return fail("Files are missing: %s. The dump looks incomplete; dump or extract it again." % ", ".join(missing),
                        "Incomplete")
        r.update(status="ok", title="Breath of the Wild (USA)", detail="Title %s" % tid)
    elif kind == "update":
        rpx = os.path.join(path, "code", "U-King.rpx")
        if ver != UPDATE_VERSION:
            return fail("This is update v%s. The Mac port needs update v%d (game version 1.5.0), the last one."
                        % (ver, UPDATE_VERSION), "Wrong version")
        if not os.path.isfile(rpx):
            return fail("code/U-King.rpx is missing. The update dump is incomplete.", "Incomplete")
        if file_sha256(rpx) != RPX_SHA256:
            return fail("code/U-King.rpx differs from the v208 game code (it may be modified or damaged). "
                        "Dump the update again; mods go in later, not into the dump.", "Unexpected game code")
        r.update(status="ok", title="Version %d (game version 1.5.0)" % ver, detail="Title %s, game code verified" % tid)
    else:
        if not os.path.isdir(os.path.join(path, "content", "0010")):
            return fail("content/0010 is missing; the DLC dump looks incomplete.", "Incomplete")
        packs = [p for p, n in (("0010", "The Master Trials"), ("0011", "The Champions' Ballad")) if os.path.isdir(os.path.join(path, "content", p))]
        r.update(status="ok", title="Version %s" % ver, detail="Title %s" % tid)
        if ver is not None and ver < DLC_LATEST:
            r["status"] = "warn"
            r["messages"].append("This is DLC v%d; v%d is the last one (with The Champions' Ballad). It may still work."
                                 % (ver, DLC_LATEST))
        elif "0011" not in packs:
            r["status"] = "warn"
            r["messages"].append("content/0011 (The Champions' Ballad) is missing.")
    if r["status"] == "ok":
        r["size"] = folder_size(path)
    return r


def folder_size(path):
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(d, f)).st_size
            except OSError:
                pass
    return total


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def scan(top, depth=4):
    """Finds BotW title folders under top (an extracted dump, a folder of dumps or Cemu's mlc01)."""
    found = {"base": [], "update": [], "dlc": []}
    top = os.path.abspath(top)

    def visit(d, level):
        try:
            names = os.listdir(d)
        except OSError:
            return
        if os.path.isfile(os.path.join(d, "code", "app.xml")):
            tid, _ = read_title(d)
            if tid and tid[8:] == TITLE_LOW and tid[:8] in KINDS:
                found[KINDS[tid[:8]]].append(d)
            return
        if level >= depth:
            return
        for n in sorted(names):
            if n.startswith(".") or n in ("content", "code", "meta"):
                continue
            p = os.path.join(d, n)
            if os.path.isdir(p) and not os.path.islink(p):
                visit(p, level + 1)

    visit(top, 0)
    # Cemu: mlc01/usr/title/<hi>/<lo>
    mlc = os.path.join(top, "usr", "title")
    if os.path.isdir(mlc):
        for hi, kind in KINDS.items():
            p = os.path.join(mlc, hi.lower(), TITLE_LOW.lower())
            if os.path.isdir(p) and p not in found[kind]:
                found[kind].append(p)
    return {k: (v[0] if v else "") for k, v in found.items()}


# ---------------------------------------------------------------- saves

def save_user_dir(path):
    """The folder that holds the save slots (0..5, option.sav): Cemu's user/80000001 or what leads to it."""
    def is_user(d):
        if not os.path.isdir(d):
            return False
        if os.path.isfile(os.path.join(d, "option.sav")):
            return True
        return any(os.path.isfile(os.path.join(d, str(i), "game_data.sav")) for i in range(6))

    cands = [path,
             os.path.join(path, "user"),
             os.path.join(path, "usr", "save", "00050000", TITLE_LOW.lower(), "user"),
             os.path.join(path, "save", "00050000", TITLE_LOW.lower(), "user"),
             os.path.join(path, "00050000", TITLE_LOW.lower(), "user"),
             os.path.join(path, TITLE_LOW.lower(), "user")]
    for c in cands:
        if is_user(c):
            return c
        if os.path.isdir(c):
            for n in sorted(os.listdir(c)):  # user/8000000x: Cemu's account folders
                if re.match(r"^8[0-9a-fA-F]{7}$", n) and is_user(os.path.join(c, n)):
                    return os.path.join(c, n)
    return None


def check_save(path):
    r = {"kind": "save", "path": path or "", "status": "empty", "title": "Optional", "detail": "", "messages": []}
    if not path:
        return r
    if not os.path.isdir(path):
        r.update(status="error", title="Not a folder")
        r["messages"].append("Choose the save folder, for example Cemu's mlc01/usr/save/00050000/101c9400/user/80000001.")
        return r
    path = os.path.abspath(path)
    u = save_user_dir(path)
    if not u:
        r.update(status="error", title="No save found")
        r["messages"].append("No Breath of the Wild save is in this folder (no option.sav and no 0..5 slots with "
                             "game_data.sav). In Cemu it is mlc01/usr/save/00050000/101c9400/user/80000001.")
        return r
    slots, newest = [], 0
    for i in range(6):
        g = os.path.join(u, str(i), "game_data.sav")
        if os.path.isfile(g):
            slots.append(i)
            newest = max(newest, os.path.getmtime(g))
    r.update(status="ok", user_dir=u, slots=slots,
             title="Save with %d slot%s" % (len(slots), "" if len(slots) == 1 else "s") if slots else "Settings only",
             detail=("last saved %s" % time.strftime("%Y-%m-%d %H:%M", time.localtime(newest))) if newest else u)
    if existing_save():
        r["status"] = "warn"
        r["messages"].append("A save already exists for the Mac app; it is backed up next to it before the import.")
    return r


def existing_save():
    u = os.path.join(SAVE, "user")
    return os.path.isdir(u) and any(not n.startswith(".") for n in os.listdir(u))


def import_save(src_user, log):
    os.makedirs(SAVE, exist_ok=True)
    if existing_save():
        backup = os.path.join(SUPPORT, "save.backup-" + time.strftime("%Y%m%d-%H%M%S"))
        os.makedirs(backup)
        for n in ("user", "common"):
            if os.path.exists(os.path.join(SAVE, n)):
                shutil.move(os.path.join(SAVE, n), os.path.join(backup, n))
        log("Backed up the previous save to %s" % backup)
    shutil.copytree(src_user, os.path.join(SAVE, "user"), dirs_exist_ok=True)  # empty or moved away above
    common = os.path.join(os.path.dirname(src_user), "common")  # Cemu: user/common next to user/80000001
    if os.path.basename(os.path.dirname(src_user)) == "user" and os.path.isdir(common):
        shutil.rmtree(os.path.join(SAVE, "common"), ignore_errors=True)
        shutil.copytree(common, os.path.join(SAVE, "common"))
    log("Imported the save from %s" % src_user)


# ---------------------------------------------------------------- tools

def load_kit():
    p = os.path.join(KIT, "kit.json")
    if not os.path.isfile(p):
        return None
    with open(p) as f:
        return json.load(f)


def tool_dirs():
    return [d for d in os.environ.get("PATH", "").split(os.pathsep) if d] + [
        "/opt/homebrew/bin", "/usr/local/bin", "/Applications/CMake.app/Contents/bin",
        os.path.join(HOME, "Applications", "CMake.app", "Contents", "bin"), "/opt/local/bin"]


def find_tool(name):
    for d in tool_dirs():
        p = os.path.join(d, name)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def clt_ok():
    try:
        subprocess.run(["/usr/bin/xcrun", "--find", "clang"], check=True, capture_output=True, timeout=60)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def requirements(copy_game_bytes=0):
    items = []

    def add(name, ok, detail, fix=""):
        items.append({"name": name, "status": "ok" if ok is True else ("warn" if ok == "warn" else "error"),
                      "detail": detail, "fix": fix})

    mac = platform.mac_ver()[0]
    major = int(mac.split(".")[0]) if mac else 0
    add("macOS", major >= 13 or "warn", "macOS %s" % mac, "" if major >= 13 else "macOS 13 Ventura or newer is needed.")
    arm = platform.machine() == "arm64"
    add("Apple silicon", arm or "warn", "Apple silicon" if arm else platform.machine(),
        "" if arm else "The port is built and tested on Apple silicon; an Intel Mac is not supported.")
    ok = clt_ok()
    add("Command Line Tools", ok, "compiler found" if ok else "not installed",
        "" if ok else "Apple's free Command Line Tools provide the compiler. Click Install, or run: xcode-select --install")
    if not load_kit():  # a source checkout builds the runtime too
        cm = find_tool("cmake")
        add("CMake", bool(cm), cm or "not found",
            "" if cm else "Install CMake with Homebrew (brew install cmake ninja) or the CMake app from cmake.org.")
        nj = find_tool("ninja")
        add("Ninja (optional)", True if nj else "warn", nj or "not found: the build uses make instead (a little slower)",
            "" if nj else "Optional: brew install ninja")
    need = 3 * (1 << 30) + copy_game_bytes
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    free = shutil.disk_usage(os.path.dirname(CACHE)).free
    add("Disk space", free >= need, "%s free, %s needed" % (gib(free), gib(need)),
        "" if free >= need else "Free up space on the startup disk.")
    return items


def gib(n):
    return "%.1f GB" % (n / 1e9)


# ---------------------------------------------------------------- install

class Cancelled(Exception):
    pass


class Installer:
    def __init__(self, args, json_out):
        self.args, self.json = args, json_out
        self.child = None
        self.cancelled = False
        os.makedirs(CACHE, exist_ok=True)
        os.makedirs(LOGS, exist_ok=True)
        self.logf = open(os.path.join(LOGS, "installer.log"), "w")
        self.jobs = args.jobs or os.cpu_count() or 4
        self.steps = []
        self.started = {}
        self.finished = {}
        self.estimates = {}
        self.current = None
        self.fraction = 0.0
        self.step_eta = None
        self.lock = threading.Lock()
        self.kit = load_kit()
        self.children = set()

    # -- output
    def emit(self, **ev):
        if self.json:
            with self.lock:
                sys.stdout.write(json.dumps(ev) + "\n")
                sys.stdout.flush()
        elif ev["event"] in ("error", "done"):
            print("%s: %s" % (ev["event"], ev.get("message") or ev.get("app")), flush=True)

    def log(self, text):
        self.logf.write(text + "\n")
        self.logf.flush()
        if self.json:
            self.emit(event="log", text=text)
        else:
            print(text, flush=True)

    def progress(self, fraction, detail="", eta=None):
        self.fraction = max(0.0, min(1.0, fraction))
        if eta is None and self.fraction > 0.02:
            el = time.time() - self.started[self.current]
            eta = el / self.fraction * (1 - self.fraction)
        self.step_eta = eta
        self.emit(event="progress", id=self.current, fraction=self.fraction, detail=detail,
                  eta=eta, overall=self.overall(), overall_eta=self.overall_eta())

    def overall(self):
        total = sum(self.estimates.values()) or 1
        done = sum(self.estimates[s] for s in self.finished)
        if self.current and self.current not in self.finished:
            done += self.estimates[self.current] * self.fraction
        return done / total

    def overall_eta(self):
        rest = 0.0
        for s, _ in self.steps:
            if s in self.finished:
                continue
            if s == self.current:
                el = time.time() - self.started[s]
                rest += self.step_eta if self.step_eta is not None else max(self.estimates[s] - el, 5)
            else:
                rest += self.estimates[s]
        return rest

    def step(self, sid):
        self.current = sid
        self.fraction, self.step_eta = 0.0, None
        self.started[sid] = time.time()
        self.emit(event="step", id=sid, state="running", title=dict(self.steps)[sid])
        self.log("==> %s" % dict(self.steps)[sid])

    def step_done(self, sid, state="done", detail=""):
        self.finished[sid] = time.time() - self.started.get(sid, time.time())
        self.estimates[sid] = self.estimates.get(sid, 1)
        self.emit(event="step", id=sid, state=state, title=dict(self.steps)[sid], detail=detail,
                  seconds=self.finished[sid], overall=self.overall(), overall_eta=self.overall_eta())

    # -- child processes
    def run(self, cmd, cwd=None, on_line=None, env=None, show=None):
        if self.cancelled:
            raise Cancelled()
        self.log("$ " + (show or " ".join(sh_quote(c) for c in cmd)))
        self.child = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, bufsize=1, universal_newlines=True, errors="replace")
        tail = []
        for line in self.child.stdout:
            line = line.rstrip("\n")
            tail = (tail + [line])[-40:]
            if on_line and on_line(line):
                continue
            self.log(line)
        rc = self.child.wait()
        self.child = None
        if self.cancelled:
            raise Cancelled()
        if rc != 0:
            raise StepError("%s failed (exit code %d)." % (os.path.basename(cmd[0]), rc), "\n".join(tail))

    def cancel(self, *_):
        self.cancelled = True
        for c in [self.child] + list(self.children):
            if c and c.poll() is None:
                try:
                    c.terminate()
                except OSError:
                    pass

    # -- the steps
    def main(self):
        a = self.args
        plan = [("check", "Checking your game files and tools")]
        if a.copy_game:
            plan.append(("copy", "Copying the game files"))
        plan.append(("recompile", "Translating the game code"))
        if self.kit:
            plan += [("compile", "Compiling the game"), ("link", "Linking the game")]
        else:
            plan += [("configure", "Preparing the build"), ("compile", "Compiling the game")]
        plan.append(("bundle", "Creating %s" % APP_NAME))
        if a.save:
            plan.append(("save", "Importing your save"))
        if not a.keep_build:
            plan.append(("cleanup", "Removing build files"))
        self.steps = plan
        cores = max(1, min(self.jobs, os.cpu_count() or 1))
        self.estimates = {"check": 3, "copy": 60, "recompile": EST_RECOMP, "configure": 15,
                          "compile": EST_COMPILE_CPU / (cores * 0.85), "link": 20, "bundle": 5, "save": 2, "cleanup": 5}
        self.emit(event="plan", steps=[{"id": s, "title": t} for s, t in plan], overall_eta=self.overall_eta())
        signal.signal(signal.SIGTERM, self.cancel)
        signal.signal(signal.SIGINT, self.cancel)
        self.log("BOTWRecomp installer %s, %s (%s)" % (version(), ROOT, "prebuilt runtime" if self.kit else "source build"))
        try:
            self.do_check()
            if a.copy_game:
                self.do_copy()
            self.do_recompile()
            if self.kit:
                self.do_compile_kit()
                self.do_link_kit()
            else:
                self.do_configure()
                self.do_compile()
            app = self.do_bundle()
            if a.save:
                self.step("save")
                import_save(self.save_user, self.log)
                self.step_done("save")
            if not a.keep_build:
                self.step("cleanup")
                shutil.rmtree(os.path.join(CACHE, "gen"), ignore_errors=True)
                shutil.rmtree(os.path.join(CACHE, "rt"), ignore_errors=True)
                shutil.rmtree(os.path.join(CACHE, "obj"), ignore_errors=True)
                self.step_done("cleanup")
            self.log("Done: %s" % app)
            self.emit(event="done", app=app, log=self.logf.name, save=SAVE)
            return 0
        except Cancelled:
            self.log("Cancelled.")
            self.emit(event="error", step=self.current, title="Cancelled", message="The installation was cancelled.",
                      log=self.logf.name, cancelled=True)
            return 130
        except StepError as e:
            self.log("ERROR: %s" % e.message)
            self.emit(event="step", id=self.current, state="failed", title=dict(self.steps).get(self.current, ""))
            self.emit(event="error", step=self.current, title=e.title or "The installation could not finish",
                      message=e.message, tail=e.tail, log=self.logf.name)
            return 1
        except Exception as e:  # unexpected: still report it
            import traceback
            self.log(traceback.format_exc())
            self.emit(event="step", id=self.current, state="failed", title=dict(self.steps).get(self.current, ""))
            self.emit(event="error", step=self.current, title="Something went wrong",
                      message="%s: %s" % (type(e).__name__, e), log=self.logf.name)
            return 1

    def do_check(self):
        a = self.args
        self.step("check")
        problems = []
        for kind, path in (("base", a.base), ("update", a.update), ("dlc", a.dlc)):
            if kind == "dlc" and not path:
                continue
            r = check_title(path, kind)
            if r["status"] == "error" or r["status"] == "empty":
                problems.append("%s: %s" % (KIND_NAMES[kind].capitalize(), " ".join(r["messages"]) or r["title"]))
            else:
                self.log("%s: %s (%s)" % (KIND_NAMES[kind], r["title"], path))
                for m in r["messages"]:
                    self.log("  note: " + m)
        if a.save:
            r = check_save(a.save)
            if r["status"] == "error":
                problems.append("Save: " + " ".join(r["messages"]))
            else:
                self.save_user = r["user_dir"]
        copy_bytes = 0
        if a.copy_game:
            copy_bytes = sum(folder_size(p) for p in (a.base, a.update, a.dlc) if p)
        for req in requirements(copy_bytes):
            if req["status"] == "error":
                problems.append("%s: %s. %s" % (req["name"], req["detail"], req["fix"]))
        if self.kit:
            if not all(os.path.isfile(os.path.join(KIT, n)) for n in self.kit["files"]):
                problems.append("The installer is incomplete (%s); download it again." % KIT)
        elif not os.path.isfile(os.path.join(ROOT, "CMakeLists.txt")) or not os.path.isdir(os.path.join(ROOT, "runtime")):
            problems.append("The port's source folder is incomplete (%s); download the release again." % ROOT)
        dest = os.path.abspath(os.path.expanduser(a.dest))
        try:
            os.makedirs(dest, exist_ok=True)
            probe = tempfile.NamedTemporaryFile(dir=dest, prefix=".botw-write-test")
            probe.close()
        except OSError as e:
            problems.append("The app can't be written to %s: %s" % (dest, e.strerror or e))
        if problems:
            raise StepError("\n".join(problems), title="Some files or tools need attention")
        self.step_done("check")

    def do_copy(self):
        a = self.args
        self.step("copy")
        jobs = [(a.base, os.path.join(GAME_COPY, "Game")), (a.update, os.path.join(GAME_COPY, "Update"))]
        if a.dlc:
            jobs.append((a.dlc, os.path.join(GAME_COPY, "DLC")))
        files = []
        for src, dst in jobs:
            if os.path.realpath(src) == os.path.realpath(dst):
                continue
            for d, _, names in os.walk(src):
                for n in names:
                    s = os.path.join(d, n)
                    files.append((s, os.path.join(dst, os.path.relpath(s, src)), os.path.getsize(s)))
        total = sum(f[2] for f in files) or 1
        done, last = 0, 0.0
        t0 = time.time()
        for s, d, size in files:
            if self.cancelled:
                raise Cancelled()
            try:
                st = os.stat(d)
                same = st.st_size == size and int(st.st_mtime) == int(os.path.getmtime(s))
            except OSError:
                same = False
            if not same:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d + ".part")
                os.replace(d + ".part", d)
            done += size
            if time.time() - last > 0.25:
                last = time.time()
                rate = done / max(last - t0, 0.01)
                self.progress(done / total, "%s of %s" % (gib(done), gib(total)), (total - done) / rate)
        self.paths = {"base": jobs[0][1], "update": jobs[1][1], "dlc": jobs[2][1] if a.dlc else ""}
        self.step_done("copy")

    def do_recompile(self):
        a = self.args
        self.step("recompile")
        rpx = os.path.join(a.update, "code", "U-King.rpx")
        gen = os.path.join(CACHE, "gen")
        stamp = recomp_stamp(rpx)
        stamp_file = os.path.join(gen, ".installer-stamp")
        if os.path.isfile(stamp_file) and open(stamp_file).read() == stamp:
            self.log("The game code was translated before with the same translator; reusing %s" % gen)
            self.step_done("recompile", "skipped", "already up to date")
            return
        tmp = gen + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        t0 = time.time()

        def on_line(line):
            m = re.match(r"^@progress (\w+) (\d+) (\d+)$", line)
            if not m:
                return False
            phase, n, total = m.group(1), int(m.group(2)), int(m.group(3))
            if phase == "analyze":
                el = time.time() - t0
                self.progress(min(0.55, el / EST_RECOMP), "Analysing the code", max(EST_RECOMP - el, 3))
            else:
                self.progress(0.55 + 0.45 * n / max(total, 1), "Translated %s of %s functions" % (
                    "{:,}".format(n), "{:,}".format(total)))
            return True

        self.run([sys.executable, "-u", os.path.abspath(__file__), "recomp", rpx, tmp], cwd=ROOT, on_line=on_line)
        with open(os.path.join(tmp, ".installer-stamp"), "w") as f:
            f.write(stamp)
        shutil.rmtree(gen, ignore_errors=True)
        os.replace(tmp, gen)
        self.step_done("recompile")

    def do_configure(self):
        self.step("configure")
        rt = os.path.join(CACHE, "rt")
        cmake = find_tool("cmake")
        ninja = find_tool("ninja")
        cache_file = os.path.join(rt, "CMakeCache.txt")
        if os.path.isfile(cache_file):
            gen_ok = ("GEN_DIR:PATH=%s\n" % os.path.join(CACHE, "gen")) in open(cache_file).read()
            src_ok = ("CMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % ROOT) in open(cache_file).read()
            if not (gen_ok and src_ok) or ("Ninja" in open(cache_file).read()) != bool(ninja):
                shutil.rmtree(rt)
        cmd = [cmake, "-S", ROOT, "-B", rt, "-DCMAKE_BUILD_TYPE=Release", "-DWWHD_RENDERER=METAL",
               "-DGEN_DIR=" + os.path.join(CACHE, "gen"), "-DWWHD_EXTRACTOR=OFF"]
        if ninja:
            cmd += ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja]
        self.cmake = cmake
        self.run(cmd, cwd=ROOT, env=build_env())
        self.step_done("configure")

    def do_compile(self):
        self.step("compile")
        rt = os.path.join(CACHE, "rt")
        state = {"last": 0.0, "done": 0.0, "seen": 0}
        n_gen = len([n for n in os.listdir(os.path.join(CACHE, "gen")) if n.startswith("code_")])

        def on_line(line):
            m = re.match(r"^\[(\d+)/(\d+)\] (.*)$", line) or re.match(r"^\[\s*(\d+)%\] (.*)$", line)
            if not m:
                return False
            if len(m.groups()) == 3:
                n, total, what = int(m.group(1)), int(m.group(2)), m.group(3)
                # ninja counts every object the same; a file of generated game code takes ~4x longer
                state["done"] += GEN_WEIGHT if "gamecode.dir" in what else 1.0
                state["seen"] += "gamecode.dir" in what
                frac = min(state["done"] / (n_gen * GEN_WEIGHT + max(total - n_gen, 1)), 1.0)
            else:
                frac, what = int(m.group(1)) / 100.0, m.group(2)
            self.logf.write(line + "\n")
            if time.time() - state["last"] > 0.3 or frac >= 1:
                state["last"] = time.time()
                name = os.path.basename(re.sub(r"^.* (\S+?)(\.o|\.obj)?$", r"\1", what))
                if "gamecode.dir" in what:
                    name = "game code, part %d of %d" % (state["seen"], n_gen)
                elif what.startswith("Linking"):
                    name = "linking " + name
                self.progress(frac, name)
            if self.json:
                self.emit(event="log", text=line)
            return True

        self.run([self.cmake, "--build", rt, "--target", "wwhd", "--parallel", str(self.jobs)], cwd=ROOT,
                 on_line=on_line, env=build_env())
        self.step_done("compile")

    def do_compile_kit(self):
        """Compiles the generated code with the flags the prebuilt runtime was built with (kit.json)."""
        self.step("compile")
        gen, obj = os.path.join(CACHE, "gen"), os.path.join(CACHE, "obj")
        os.makedirs(obj, exist_ok=True)
        flags = self.kit["cflags"] + ["-I", os.path.join(KIT, "include"), "-I", gen]
        flag_stamp = hashlib.sha256(json.dumps(flags).encode()).hexdigest()
        stamp_file = os.path.join(obj, ".flags")
        if not os.path.isfile(stamp_file) or open(stamp_file).read() != flag_stamp:
            shutil.rmtree(obj)
            os.makedirs(obj)
            with open(stamp_file, "w") as f:
                f.write(flag_stamp)
        sources = sorted(n for n in os.listdir(gen) if n.endswith(".c"))
        weight = {n: (GEN_WEIGHT if n.startswith("code_") else 1.0) for n in sources}
        todo = [n for n in sources if not up_to_date(os.path.join(obj, n + ".o"), os.path.join(gen, n))]
        total = sum(weight.values())
        done = [total - sum(weight[n] for n in todo), 0]
        self.objects = [os.path.join(obj, n + ".o") for n in sources]
        self.log("%d of %d files to compile" % (len(todo), len(sources)))
        errors = []
        queue = list(todo)
        lock = threading.Lock()

        def worker():
            while True:
                with lock:
                    if not queue or errors or self.cancelled:
                        return
                    n = queue.pop(0)
                out = os.path.join(obj, n + ".o")
                cmd = ["/usr/bin/clang"] + flags + ["-c", os.path.join(gen, n), "-o", out + ".tmp"]
                t0 = time.time()
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
                with lock:
                    self.children.add(p)
                text = p.communicate()[0].decode(errors="replace")
                with lock:
                    self.children.discard(p)
                    if p.returncode != 0:
                        if not self.cancelled:
                            errors.append((n, text))
                        return
                    os.replace(out + ".tmp", out)
                    done[0] += weight[n]
                    done[1] += 1
                    self.log("[%d/%d] %s (%.0f s)" % (done[1], len(todo), n, time.time() - t0))
                    self.progress(done[0] / total, "game code, part %d of %d" % (done[1], len(todo)))

        threads = [threading.Thread(target=worker) for _ in range(self.jobs)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        if self.cancelled:
            raise Cancelled()
        if errors:
            n, text = errors[0]
            raise StepError("The compiler failed on %s." % n, "\n".join(text.splitlines()[-40:]))
        self.step_done("compile")

    def do_link_kit(self):
        self.step("link")
        self.exe = os.path.join(CACHE, "obj", "wwhd")
        cmd = ["/usr/bin/clang++"]
        for a in self.kit["link"]:
            if a == "@GAMECODE@":
                cmd += self.objects
            else:
                cmd.append(a.replace("@KIT@", KIT).replace("@OUT@", self.exe + ".tmp"))
        self.progress(0.1, "about half a minute")
        self.run(cmd, cwd=CACHE, env=build_env(),
                 show="clang++ -o %s <%d game code objects> <the prebuilt runtime>" % (self.exe, len(self.objects)))
        os.replace(self.exe + ".tmp", self.exe)
        self.step_done("link")

    def do_bundle(self):
        a = self.args
        self.step("bundle")
        paths = getattr(self, "paths", None) or {"base": a.base, "update": a.update, "dlc": a.dlc or ""}
        dest = os.path.abspath(os.path.expanduser(a.dest))
        app = os.path.join(dest, APP_NAME)
        exe = getattr(self, "exe", None) or os.path.join(CACHE, "rt", "wwhd")
        tmp = tempfile.mkdtemp(prefix=".botw-app-", dir=dest)
        try:
            c = os.path.join(tmp, APP_NAME, "Contents")
            os.makedirs(os.path.join(c, "MacOS"))
            os.makedirs(os.path.join(c, "Resources"))
            shutil.copy2(exe, os.path.join(c, "MacOS", "wwhd"))
            if make_icon(os.path.join(paths["base"], "meta", "iconTex.tga"), os.path.join(c, "Resources", "icon.icns"), tmp):
                self.log("Icon made from your dump's meta/iconTex.tga")
            with open(os.path.join(c, "Info.plist"), "wb") as f:
                plistlib.dump({
                    "CFBundleName": "Breath of the Wild", "CFBundleDisplayName": "Breath of the Wild",
                    "CFBundleIdentifier": BUNDLE_ID, "CFBundleExecutable": "wwhd", "CFBundleIconFile": "icon",
                    "CFBundlePackageType": "APPL", "CFBundleVersion": version(), "CFBundleShortVersionString": "1.5.0",
                    "LSMinimumSystemVersion": "13.0", "NSHighResolutionCapable": True,
                    "GCSupportsControllerUserInteraction": True,
                    "LSApplicationCategoryType": "public.app-category.action-games",
                    "LSSupportsGameMode": True, "GCSupportsGameMode": True,  # Game Mode in full screen (macOS 14+)
                    "BotWInstalledFrom": ROOT,
                }, f)
            self.progress(0.5, "Signing")
            subprocess.run(["/usr/bin/codesign", "--force", "--deep", "-s", "-", os.path.join(tmp, APP_NAME)],
                           capture_output=True)
            if os.path.exists(app):
                old = os.path.join(tmp, "old.app")
                os.rename(app, old)
            os.rename(os.path.join(tmp, APP_NAME), app)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        # where the app finds everything (per user, independent of this source folder)
        os.makedirs(SUPPORT, exist_ok=True)
        os.makedirs(os.path.join(SAVE, "user"), exist_ok=True)
        cfg = {"base": os.path.abspath(paths["base"]), "update": os.path.abspath(paths["update"]),
               "dlc": os.path.abspath(paths["dlc"]) if paths["dlc"] else "", "save": SAVE,
               "app": app, "source": ROOT, "installed": time.strftime("%Y-%m-%d %H:%M:%S")}
        with open(CONFIG + ".tmp", "wb") as f:
            plistlib.dump(cfg, f)
        os.replace(CONFIG + ".tmp", CONFIG)
        self.log("Game paths written to %s" % CONFIG)
        subprocess.run(["/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/"
                        "Support/lsregister", "-f", app], capture_output=True)
        self.step_done("bundle")
        return app


class StepError(Exception):
    def __init__(self, message, tail="", title=""):
        super().__init__(message)
        self.message, self.tail, self.title = message, tail, title


def build_env():
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(dict.fromkeys(tool_dirs() + ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]))
    return env


def recomp_stamp(rpx):
    h = hashlib.sha256()
    h.update(file_sha256(rpx).encode())
    for d in (os.path.join(ROOT, "tools", "recomp"), os.path.join(ROOT, "tools")):
        for n in sorted(os.listdir(d)):
            if n.endswith(".py") and (d.endswith("recomp") or n in ("probe_recomp.py", "botw_fps_patches.py", "rpx.py")):
                with open(os.path.join(d, n), "rb") as f:
                    h.update(n.encode() + f.read())
    return h.hexdigest()


def up_to_date(target, source):
    try:
        return os.path.getmtime(target) >= os.path.getmtime(source)
    except OSError:
        return False


def version():
    kit = load_kit()
    return kit["version"] if kit else git_rev()


def git_rev():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True, timeout=10).stdout.strip() or "source"
    except (OSError, subprocess.SubprocessError):
        return "source"


def sh_quote(s):
    return s if re.match(r"^[\w./=:+-]+$", s) else "'" + s.replace("'", "'\\''") + "'"


def make_icon(tga, icns, tmp):
    if not os.path.isfile(tga):
        return False
    png, iconset = os.path.join(tmp, "icon.png"), os.path.join(tmp, "i.iconset")
    os.makedirs(iconset, exist_ok=True)
    if subprocess.run(["/usr/bin/sips", "-s", "format", "png", tga, "--out", png], capture_output=True).returncode:
        return False
    for s in (16, 32, 128, 256, 512):
        for scale, suffix in ((1, ""), (2, "@2x")):
            subprocess.run(["/usr/bin/sips", "-z", str(s * scale), str(s * scale), png, "--out",
                            os.path.join(iconset, "icon_%dx%d%s.png" % (s, s, suffix))], capture_output=True)
    return subprocess.run(["/usr/bin/iconutil", "-c", "icns", iconset, "-o", icns], capture_output=True).returncode == 0


# ---------------------------------------------------------------- the translator, run as a child process

def cmd_recomp(rpx, out):
    """Runs tools/probe_recomp.py's translation (the one build/gen of the dev checkout comes from) and
    reports progress as '@progress PHASE N TOTAL' lines."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
    import probe_recomp  # noqa: E402  (also turns off WWHD's hooks)
    import recomp  # noqa: E402

    stop = threading.Event()

    def ticker():
        while not stop.wait(1.0):
            print("@progress analyze 0 0", flush=True)
    threading.Thread(target=ticker, daemon=True).start()
    r = probe_recomp.make_recompiler(rpx)
    stop.set()
    total = len(r.sorted_entries)
    emit = r.emit_function
    count = [0]

    def counting(start):
        count[0] += 1
        if count[0] % 2000 == 0 or count[0] == total:
            print("@progress emit %d %d" % (count[0], total), flush=True)
        return emit(start)
    r.emit_function = counting
    r.run(out, FUNCS_PER_FILE)


# ---------------------------------------------------------------- command line

def main(argv):
    ap = argparse.ArgumentParser(description="Build Breath of the Wild for Mac from your own Wii U dump.")
    ap.add_argument("--json", action="store_true", help="machine-readable output (the installer app)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("dir")
    c = sub.add_parser("check")
    for n in ("base", "update", "dlc", "save"):
        c.add_argument("--" + n, default="")
    sub.add_parser("requirements").add_argument("--copy-bytes", type=int, default=0)
    i = sub.add_parser("install")
    for n in ("base", "update", "dlc", "save"):
        i.add_argument("--" + n, default="")
    i.add_argument("--dest", default=os.path.join(HOME, "Applications"))
    i.add_argument("--copy-game", action="store_true", help="copy the game files to ~/Library/Application Support/BotW/game")
    i.add_argument("--keep-build", action="store_true", help="keep the generated code and build for a faster rebuild")
    i.add_argument("--jobs", type=int, default=0)
    r = sub.add_parser("recomp")
    r.add_argument("rpx")
    r.add_argument("out")
    a = ap.parse_args(argv)

    def out(obj):
        if a.json:
            print(json.dumps(obj))
        else:
            print(json.dumps(obj, indent=1))

    if a.cmd == "scan":
        out(scan(a.dir))
    elif a.cmd == "check":
        res = {}
        for kind in ("base", "update", "dlc"):
            res[kind] = check_title(getattr(a, kind), kind)
        res["save"] = check_save(a.save)
        res["existing_save"] = existing_save()
        out(res)
    elif a.cmd == "requirements":
        out(requirements(a.copy_bytes))
    elif a.cmd == "install":
        if not a.base or not a.update:
            ap.error("install needs --base and --update")
        return Installer(a, a.json).main()
    elif a.cmd == "recomp":
        cmd_recomp(a.rpx, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
