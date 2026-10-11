#!/usr/bin/env python3
"""Feasibility probe: run ZeldaWWHDRecomp's PPC->C recompiler on BotW's U-King.rpx.

usage: probe_recomp.py path/to/U-King.rpx OUTDIR

Uses tools/recomp (from ZeldaWWHDRecomp), but with an identity "canonical" build and no
WWHD hooks (those addresses belong to Wind Waker HD).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "recomp"))
import builds as game_builds  # noqa: E402
import recomp  # noqa: E402

game_builds.hook_files = lambda: []


def botw_build(rpx):
    return game_builds.Build({
        "name": "USA", "title_id": "00050000101C9400",
        "rpx_sha256": game_builds.file_sha256(rpx),
    })


if __name__ == "__main__":
    rpx, out = sys.argv[1], sys.argv[2]
    recomp.Recompiler(rpx, botw_build(rpx)).run(out, 30000)
