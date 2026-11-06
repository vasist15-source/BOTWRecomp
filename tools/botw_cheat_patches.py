"""v208 cheats the player switches on and off in game (Settings > Cheats), applied only after the
RPX identity check.

The sites are those of the v208 patches in Cemu graphic packs' Breath of the Wild Cheats packs and
its Weather mod (src/BreathOfTheWild/Cheats and Mods/Weather of
https://github.com/cemu-project/cemu_graphic_packs, graphic packs v989, CC0-1.0). Cemu writes the
patched instruction into the game, so a change needs a restart; here the translated code keeps the
game's own instruction and asks a runtime switch first (runtime/include/botw_cheats.h, set by
runtime/src/mods/cheats.cpp). With a cheat off its site runs exactly the original code, so the
switches apply at once.

Where this differs from the packs:
- Durability: the pack loads its multiplier in place of `mtctr r10; fmr f31, f1` (024ACBF0), which
  also drops the next virtual call's target. Here only the loss handed to the weapon's life is zeroed
  (f1 at 024ACC60), and by the weapon's type (WeaponBase + 0x938: 0-3 swords, spears and bows, 4
  shields), so weapons and shields are separate switches.
- Urbosa's Fury: the pack's nop also skips the compare the next branch reads (addic.); here the
  count is "decremented" by 0, so cr0 still says uses are left.
- Weather: the pack drops the "weather unchanged" branch and maps every stored value; here the
  current weather is compared with the forced one and the forced one stored, so a change waits for
  the game's own transition as before and the desired weather (r26) stays the game's.
- No Blood Moon is not a pack: TimeMgr's "bloody day" check (0365ECBC, FirstTouchdown and the timer
  at +0xA8 over 2520 = 7 * 360) answers no, so no day is marked WM_BloodyDay and Demo011_0 never
  plays at midnight. The relief Blood Moon the game forces to free memory (BloodyMoonRelief) is
  another path and still runs. A day already marked keeps its Blood Moon.
- No random enemy spawns: the auto-placement check of Prevent Random Spawns (029FFB3C, name at
  *(r1 + 0x40)) with its "all enemies" choice: an Enemy_ actor is reported as already placed.
"""

NOP = 0x60000000

# address: (original instruction, switch (botw_cheats.h id) or C condition, while on: an instruction
# to run instead (NOP: none), ('instead', C) or ('after', C))
SITES = {
    # pouch arrow removal (count at item + 0x10): `count -= used` is skipped while more are left
    0x02EB6758: (0x7C180050, 'BOTW_CHEAT_ARROWS', NOP),                     # subf r0, r24, r0
    # WeaponBase (vtable 100C54AC) slot 0x22C: the durability loss passed to the weapon's life
    0x024ACC60: (0xFC20F890, 'botw_cheat_unbreakable(ld32(c->r[30] + 0x938u))',
                 ('after', 'c->f[1].ps0 = 0.0;')),                          # fmr f1, f31
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
    # TimeMgr "bloody day" check (called at each midnight by handleNewDay 0365F214): never true
    0x0365ED38: (0x3BE00001, 'BOTW_CHEAT_NO_BLOOD_MOON', NOP),              # li r31, 1
    # auto-placement: an Enemy_ actor counts as placed already (r3 = 0, cr0 = 0 vs 1: not equal)
    0x029FFB3C: (0x2C030000,
                 'botw_cheat(BOTW_CHEAT_NO_RANDOM_ENEMIES) && botw_cheat_enemy_name(ld32(c->r[1] + 0x40u))',
                 ('instead', 'c->r[3] = 0; cr_set_s(c, 0, 0, 1);')),       # cmpwi r3, 0
    # WeatherMgr (this = r30): the weather at + 0x18 becomes the wanted one (r26) once a transition
    # is over; while forced (switch = type + 1) the forced type is compared and stored in its place
    0x03668FC8: (0x7C00D040, 'BOTW_CHEAT_WEATHER',                          # cmplw r0, r26
                 ('instead', 'cr_set_u(c, 0, c->r[0], (uint32_t)botw_cheat(BOTW_CHEAT_WEATHER) - 1u);')),
    0x03668FEC: (0x9B5E0018, 'BOTW_CHEAT_WEATHER',                          # stb r26, 0x18(r30)
                 ('instead', 'st8(c->r[30] + 0x18u, (uint8_t)(botw_cheat(BOTW_CHEAT_WEATHER) - 1));')),
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
        if isinstance(change, tuple):
            how, code = change
            body = {'after': f'{original} if ({on}) {{ {code} }}',
                    'instead': f'if ({on}) {{ {code} }} else {{ {original} }}'}[how]
        elif change == NOP:
            body = f'if (!{on}) {{ {original} }}'
        else:
            body = f'if ({on}) {{ {translate(address, change)} }} else {{ {original} }}'
        lines[index] = (f'    /* BOTW_CHEAT_BEGIN {address:08X} */\n    {body} {marker}'
                        f'\n    /* BOTW_CHEAT_END {address:08X} */')
    return '#include "botw_cheats.h"\n' + '\n'.join(lines)
