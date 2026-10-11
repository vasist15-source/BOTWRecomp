#!/usr/bin/env python3
"""Caller chains in the recompiled game: who calls a function (direct calls and tail calls).

usage: callgraph.py build/gen ADDR [DEPTH]   (builds build/callers.json on first use)
"""
import glob, json, os, re, sys

gen, target = sys.argv[1], sys.argv[2].upper()
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 6
cache = os.path.join(os.path.dirname(gen.rstrip("/")), "callers.json")
if not os.path.exists(cache):
    callers = {}
    fn = re.compile(r"^void f_([0-9A-F]{8})\(")
    call = re.compile(r"\bf_([0-9A-F]{8})\(c\);.*/\* ([0-9A-F]{8}):")
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        cur = None
        for line in open(path):
            m = fn.match(line)
            if m:
                cur = m.group(1); continue
            m = call.search(line)
            if m and cur and m.group(1) != cur:
                callers.setdefault(m.group(1), set()).add(cur)
    json.dump({k: sorted(v) for k, v in callers.items()}, open(cache, "w"))
callers = json.load(open(cache))

def show(f, d, seen):
    print("  " * d + f)
    if d >= depth or f in seen: return
    seen.add(f)
    for c in callers.get(f, [])[:6]:
        show(c, d + 1, seen)
show(target, 0, set())
