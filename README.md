# botw-mac

Native macOS port of *The Legend of Zelda: Breath of the Wild* (Wii U, v1.5.0 / update v208) by
static recompilation, built on the approach and runtime of
[ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp): PowerPC -> C ahead of time,
Cafe OS reimplemented natively, GX2 mapped directly to Metal.

No game code or assets are in this repository. Everything under `build/` is generated from your
own dump.

## Layout
- `ref/ZeldaWWHDRecomp/`: upstream reference checkout (git-ignored; `git clone --depth 1 ...`)
- `tools/probe_recomp.py`: runs WWHD's recompiler unchanged on `U-King.rpx`
- `build/gen/`: generated C (about 690 MB)
- `build/missing_imports.txt`: OS/library imports BotW uses that WWHD's runtime does not implement

## Probe
```
python3 tools/probe_recomp.py "<dump>/...[Update]...[0005000e101c9400]/code/U-King.rpx" build/gen
```

See [PLAN.md](PLAN.md) for findings and the roadmap.
