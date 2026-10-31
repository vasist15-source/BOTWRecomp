#!/usr/bin/env python3
"""Translate the verified USA v208 executable, including native control-flow fixes."""
import os
import sys
import re

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "recomp"))
import builds as game_builds
import recomp
import botw_fps_patches

game_builds.hook_files = lambda: []
V208_SHA256 = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"

class BotwRecompiler(recomp.Recompiler):
    """v208 frame-time fixes; the RPX on disk and its identity stay unchanged.

    FPS++ identifies the audio floor and controller repeat as separate fixed-frame
    clocks. Keep the game's input edges, stick processing and acceleration logic;
    replace only their integer tick accumulation with native fractional clocks.
    """
    def __init__(self, rpx, build):
        super().__init__(rpx, build)
        expected = {0x037DC360: 0xC1ACE2C0, 0x030BD74C: 0x39600000,
                    0x030BD7E0: 0x4180FF7C, 0x03053DAC: 0x394AFFFF,
                    0x03053DC4: 0x394AFFFF, 0x02F62B40: 0xC00CF880,
                    0x03C70CE0: 0x4859DD9D, 0x03C70FC4: 0x4859DAB9,
                    0x0420EBB4: 0x7C7F1B78, 0x0420ECC0: 0x4E800020,
                    0x0307CCE0: 0x41820034}
        for address, instruction in expected.items():
            if self.p.word(address) != instruction:
                raise ValueError(f"Unexpected v208 timing instruction at {address:08X}")
        self.fps_sites = {}
        for address, (instruction, _, _) in botw_fps_patches.SITES.items():
            if self.p.word(address) != instruction:
                raise ValueError(f"Unexpected v208 FPS instruction at {address:08X}")
            self.fps_sites.setdefault(self.func_of(address), []).append(address)
        words = list(self.p.words)
        # lfs f13, 0x1C(r31): VFR's pending 30/fps ratio, instead of constant 1.
        words[(0x037DC360 - self.p.text_lo) // 4] = 0xC1BF001C
        # FPS++ LoadingScreen v208: a fast load can finish before the loading
        # pane reaches its fully-open state. Still send its original close
        # request; keep the preceding null-object guard and the virtual method.
        words[(0x0307CCE0 - self.p.text_lo) // 4] = 0x60000000
        self.p.words = tuple(words)

    def emit_function(self, start):
        source, count = super().emit_function(start)
        source = fix_guest_jumps(source, start)
        source = botw_fps_patches.apply(source, self.fps_sites.get(start, []))
        if start == 0x030BD694:
            lines = source.splitlines()
            first = next(i for i, line in enumerate(lines) if '/* 030BD74C:' in line)
            last = next(i for i, line in enumerate(lines) if '/* 030BD7E0:' in line)
            lines[first:last + 1] = [
                '    { extern void botw_controller_repeat(uint32_t);',
                '      botw_controller_repeat(c->r[29]); }']
            source = '\n'.join(lines)
        elif start == 0x02F62A6C:
            # Map motion has its own fixed per-frame multiplier, separate from
            # controller repeat. Preserve both position writers and zoom math.
            lines = source.splitlines()
            index = next(i for i, line in enumerate(lines) if '/* 02F62B40:' in line)
            lines[index] = ('    { extern double botw_map_frame_ratio(void); '
                            'double v = botw_map_frame_ratio(); '
                            'c->f[0].ps0 = v; c->f[0].ps1 = v; }')
            source = '\n'.join(lines)
        elif start == 0x03053BD4:
            for address, base in ((0x03053DAC, 6), (0x03053DC4, 12)):
                old = f'c->r[10] = c->r[10] + 0xFFFFFFFFu; /* {address:08X}: 394AFFFF */'
                new = ('{ extern uint32_t botw_menu_countdown(uint32_t, uint32_t); '
                       f'c->r[10] = botw_menu_countdown(c->r[{base}] + 8, c->r[10]); }}')
                if source.count(old) != 1:
                    raise ValueError('Missing menu countdown emission')
                source = source.replace(old, new)
        return source, count


def fix_guest_jumps(source, start):
    """Keep guest register restoration, but unwind the matching native C stack.

    v208 only calls its setjmp from these JPEG encode/decode entry points.
    No C++ resource-owning frame spans the guest error callback chain.
    """
    sites = {0x03C70C04: 0x03C70CE0, 0x03C70EE4: 0x03C70FC4}
    if start in sites:
        call = sites[start]
        source = '#include "botw_jump.h"\n' + source
        source = source.replace(f'    PPC_ENTER(0x{start:08X}u);',
                                f'    BotwJumpFrame jump_frame = {{0}};\n    PPC_ENTER(0x{start:08X}u);')
        lines = source.splitlines()
        index = next(i for i, line in enumerate(lines) if f'/* {call:08X}:' in line)
        lines[index:index + 1] = [
            '    botw_jump_push(&jump_frame, c, c->r[3]);', lines[index],
            '    if (setjmp(jump_frame.host)) { /* guest registers already restored */ }']
        source = '\n'.join(lines)
        # Return paths before setjmp are safe too; frame.cpu remains null.
        source = source.replace('return;', 'botw_jump_pop(&jump_frame); return;')
        source = re.sub(r'    MUSTTAIL return f_[0-9A-F]+\(c\);\n(?=\})', '', source)
    elif start == 0x0420EBB4:
        source = '#include "botw_jump.h"\n' + source
        source = source.replace('    PPC_ENTER(0x0420EBB4u);',
                                '    const uint32_t jump_buffer = c->r[3];\n    PPC_ENTER(0x0420EBB4u);')
        old = 'return; /* 0420ECC0: 4E800020 */'
        if source.count(old) != 1:
            raise ValueError('Missing guest longjmp return')
        source = source.replace(old, 'botw_jump_resume(c, jump_buffer); /* 0420ECC0: nonlocal return */')
    return source


def make_recompiler(rpx):
    return BotwRecompiler(rpx, botw_build(rpx))


def botw_build(rpx):
    digest = game_builds.file_sha256(rpx)
    if digest != V208_SHA256:
        raise ValueError("Unsupported U-King.rpx: this port requires the verified v208 executable")
    return game_builds.Build({
        "name": "USA", "title_id": "00050000101C9400", "rpx_sha256": digest,
    })


if __name__ == "__main__":
    rpx, out = sys.argv[1], sys.argv[2]
    make_recompiler(rpx).run(out, 30000)
