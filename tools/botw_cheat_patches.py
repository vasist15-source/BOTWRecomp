"""v208 cheats the player switches on and off in game (Settings > Cheats), applied only after the
RPX identity check.

The sites are those of the v208 patches in Cemu graphic packs' Breath of the Wild Cheats packs
(src/BreathOfTheWild/Cheats of https://github.com/cemu-project/cemu_graphic_packs, graphic packs
v989, CC0-1.0). Cemu writes the patched instruction into the game, so a change needs a restart; here
the translated code keeps the game's own instruction and asks a runtime switch first
(runtime/include/botw_cheats.h, set by runtime/src/mods/cheats.cpp). With a cheat off its site runs
exactly the original code, so the switches apply at once.

Where this differs from the packs:
- Durability: the pack loads its multiplier in place of `mtctr r10; fmr f31, f1` (024ACBF0), which
  also drops the next virtual call's target. Here only the loss handed to the weapon's life is zeroed
  (f1 at 024ACC60), and by the weapon's type (WeaponBase + 0x938: 0-3 swords, spears and bows, 4
  shields), so weapons and shields are separate switches.
- Urbosa's Fury: the pack's nop also skips the compare the next branch reads (addic.); here the
  count is "decremented" by 0, so cr0 still says uses are left.
"""

NOP = 0x60000000

# address: (original instruction, switch (botw_cheats.h id) or C condition, while on: an instruction
# to run instead (NOP: none) or C to run after the original)
SITES = {
    # pouch arrow removal (count at item + 0x10): `count -= used` is skipped while more are left
    0x02EB6758: (0x7C180050, 'BOTW_CHEAT_ARROWS', NOP),                     # subf r0, r24, r0
    # WeaponBase (vtable 100C54AC) slot 0x22C: the durability loss passed to the weapon's life
    0x024ACC60: (0xFC20F890, 'botw_cheat_unbreakable(ld32(c->r[30] + 0x938u))',
                 'c->f[1].ps0 = 0.0;'),                                     # fmr f1, f31
    # champion abilities on the player: uses left at + 0x16F4 (Revali), + 0x16F8 (Urbosa),
    # + 0x16FC (Daruk); Mipha's recharge time is 30 * param, -1 * param ends it at once
    0x02CE0328: (0x38A5FFFF, 'BOTW_CHEAT_REVALI', NOP),                     # addi r5, r5, -1
    0x02D741C8: (0x356BFFFF, 'BOTW_CHEAT_URBOSA', 0x356B0000),              # addic. r11, r11, -1 -> 0
    0x02D74498: (0x901D16FC, 'BOTW_CHEAT_DARUK', NOP),                      # stw r0, 0x16FC(r29)
    0x02D91DBC: (0xEC0D0032, 'BOTW_CHEAT_MIPHA', 0xEC0D0332),               # fmuls f0, f13, f0 -> f12
    # the player's damage: the new health is not stored
    0x02D452A4: (0x93FC0000, 'BOTW_CHEAT_HEARTS', NOP),                     # stw r31, 0(r28)
    # stamina: the new value is f1 without this frame's change
    0x02D90CC0: (0xEFA1F02A, 'BOTW_CHEAT_STAMINA', 0xFFA00890),             # fadds f29, f1, f30 -> fmr f29, f1
    # Master Cycle Zero: fuel keeps its value from before this frame's use
    0x020957DC: (0xFD8CFB2E, 'BOTW_CHEAT_CYCLE_FUEL', 0xFD800090),          # fsel f12, f12, f12, f31 -> fmr f12, f0
    # Master Cycle Zero: the region check (Gerudo Desert, Eldin) answers "allowed"
    0x02A32A30: (0x38600000, 'BOTW_CHEAT_CYCLE_ANYWHERE', 0x38600001),      # li r3, 0 -> 1
    # amiibo: the day an amiibo was last read is taken as never (-1)
    0x029E949C: (0x7C7D1B78, 'BOTW_CHEAT_AMIIBO', 0x7D7D5B78),              # mr r29, r3 -> r11
}


def apply(source, sites, translate):
    """Wrap the given sites of one function's C text. translate(address, word) is the C of one
    instruction, as the translator emits it (the function must be in plain form: c->r[N])."""
    if not sites:
        return source
    lines = source.splitlines()
    for address in sites:
        word, switch, change = SITES[address]
        marker = f'/* {address:08X}: {word:08X} */'
        matches = [i for i, line in enumerate(lines) if marker in line]
        if len(matches) != 1:
            raise ValueError(f'Missing or ambiguous cheat instruction {address:08X}')
        index = matches[0]
        original = lines[index][:lines[index].index(marker)].strip()
        if not original or any(k in original for k in ('goto', 'return', 'MUSTTAIL', 'L_')):
            raise ValueError(f'Cheat instruction {address:08X} is not a plain statement')
        on = switch if '(' in switch else f'botw_cheat({switch})'
        if isinstance(change, str):
            body = f'{original} if ({on}) {{ {change} }}'
        elif change == NOP:
            body = f'if (!{on}) {{ {original} }}'
        else:
            body = f'if ({on}) {{ {translate(address, change)} }} else {{ {original} }}'
        lines[index] = (f'    /* BOTW_CHEAT_BEGIN {address:08X} */\n    {body} {marker}'
                        f'\n    /* BOTW_CHEAT_END {address:08X} */')
    return '#include "botw_cheats.h"\n' + '\n'.join(lines)
