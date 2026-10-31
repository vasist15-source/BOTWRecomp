#!/usr/bin/env python3
"""Translate the verified USA v208 executable, including native control-flow fixes."""
import os
import sys
import re

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "recomp"))
import builds as game_builds
import recomp

game_builds.hook_files = lambda: []
V208_SHA256 = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"

class BotwRecompiler(recomp.Recompiler):
    def __init__(self, rpx, build):
        super().__init__(rpx, build)
        expected = {0x03C70CE0: 0x4859DD9D, 0x03C70FC4: 0x4859DAB9,
                    0x0420EBB4: 0x7C7F1B78, 0x0420ECC0: 0x4E800020}
        for address, instruction in expected.items():
            if self.p.word(address) != instruction:
                raise ValueError(f"Unexpected v208 JPEG instruction at {address:08X}")

    def emit_function(self, start):
        source, count = super().emit_function(start)
        return fix_guest_jumps(source, start), count


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
