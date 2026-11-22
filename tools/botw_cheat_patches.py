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

# View distances of Settings > Graphics (runtime/include/botw_distance.h, runtime/src/mods/distance.cpp),
# the sites of the Draw Distance mod in Cemu's graphic packs (src/BreathOfTheWild/Mods/DrawDistance,
# BotW_DrawDistance_V208, by Crementif, CC0-1.0). Where this differs from the pack:
# - Actors: the pack nops the "load balancing lowered the distance" branch and stores its multiplier,
#   so even its default (1x) turns load balancing off. Here the game's own code runs unless the value
#   differs from 1; then the compare reports "lower" and the stored value is the chosen one.
# - Objects: the pack nops a null check and loads its multiplier in place of the game's (+0x2BC of a
#   global, else a constant); here the product both paths end in (f1 = f11 * f0) takes the chosen one.
#   The culling distance compares are scaled by 2 - multiplier as the pack does, without its f1
#   clobber (f1 is not read again before the next call).
# - Trees: the billboard distance constant (0x102BC4A0, 0.5) is replaced at its load only.
# - Grass: the pack's data word (0x1030A774, 1000) is written by the runtime, not here.
DISTANCE_SITES = {
    # fcmpu cr0, f8, f9 (f9: the load-balanced multiplier); then `ble` skips `stfs f9, 0x13BC(r28)`
    0x03857F54: (0xFC084800, 'botw_dist_on(BOTW_DIST_ACTOR)',
                 ('after', '{ double v = botw_dist(BOTW_DIST_ACTOR); c->f[9].ps0 = v; c->f[9].ps1 = v; } c->cr[1] = 1;')),
    # fmuls f1, f11, f0: the object distance times its multiplier
    0x03137F00: (0xEC2B0032, 'botw_dist_on(BOTW_DIST_OBJECT)',
                 ('instead', '{ double v = ppc_to_single(c, c->f[11].ps0 * (double)botw_dist(BOTW_DIST_OBJECT)); '
                             'c->f[1].ps0 = v; c->f[1].ps1 = v; }')),
    # fmuls f13, f1, f1: the squared culling distance (two map unit loops)
    0x0313A60C: (0xEDA10072, 'botw_dist_on(BOTW_DIST_OBJECT_CULL)',
                 ('after', '{ double v = ppc_to_single(c, c->f[13].ps0 * (double)botw_dist(BOTW_DIST_OBJECT_CULL)); '
                           'c->f[13].ps0 = v; c->f[13].ps1 = v; }')),
    0x0313A678: (0xEDA10072, 'botw_dist_on(BOTW_DIST_OBJECT_CULL)',
                 ('after', '{ double v = ppc_to_single(c, c->f[13].ps0 * (double)botw_dist(BOTW_DIST_OBJECT_CULL)); '
                           'c->f[13].ps0 = v; c->f[13].ps1 = v; }')),
    # lfs f12, -0x3B60(r7): the tree billboard distance factor
    0x033CB550: (0xC187C4A0, 'botw_dist_on(BOTW_DIST_TREE)',
                 ('instead', '{ double v = botw_dist(BOTW_DIST_TREE); c->f[12].ps0 = v; c->f[12].ps1 = v; }')),
}
SITES.update(DISTANCE_SITES)

# More memory of Settings > Graphics > View distance (runtime/include/botw_memory.h, runtime/src/runtime.h
# mem::Layout): the heap sizes of Cemu's Extended Memory pack (src/BreathOfTheWild/Mods/ExtendedMemory,
# BotW_ExtendedMemory_V208, CC0-1.0), whose [RAM] mappings the runtime makes instead (MEM2 to 0xA0000000,
# a 1 GiB overlay arena). The switch is set at boot and never changes in a run. Where this differs from
# the pack:
# - "Main" and MovableMemory sizes (in MiB, 0340F31C-0340F33C): the pack sets the first choice and nops the
#   rest (compare, flag load, branch, second choice); here both choices are its value, so the flag load and
#   the compare stay the game's.
# - TexArcWork (034ECED8) is set twice in the pack; its last value (0x600) is the one it applies.
MEMORY_SITES = {
    0x02C5E660: (0x3D203B60, 'botw_more_memory()', 0x3D208B60),  # main game heap: lis r9, 0x3B60 -> 0x8B60
    0x032015E0: (0x3C60004D, 'botw_more_memory()', 0x3C600100),  # GameDataHeap: 8 + 8 MiB
    0x03201A44: (0x3C600024, 'botw_more_memory()', 0x3C600080),  # SaveArea: 8 MiB (lis r3, 0x80 ...)
    0x03201A5C: (0x38632000, 'botw_more_memory()', 0x38630000),  # ... addi r3, r3, 0)
    0x037FD6A4: (0x3CC001E0, 'botw_more_memory()', 0x3CC00500),  # HavokMainHeap: 80 MiB
    0x037FD758: (0x3C600002, 'botw_more_memory()', 0x3C600008),  # PhysicsTempLow: 512 KiB
    0x037FD6D4: (0x3C600002, 'botw_more_memory()', 0x3C600008),  # PhysicsSystem: 512 KiB
    0x02C5A208: (0x3C600140, 'botw_more_memory()', 0x3C600400),  # terrain scene
    0x0338CDBC: (0x3C600030, 'botw_more_memory()', 0x3C600080),  # ForestRenderer
    0x033C9D80: (0x3C60000C, 'botw_more_memory()', 0x3C600020),  # ForestRenderer WorkHeap
    0x030AA5F4: (0x3CE01C00, 'botw_more_memory()', 0x3CE04000),  # overlay arena root heap: 1 GiB
    0x034ED614: (0x3C600C20, 'botw_more_memory()', 0x3C601C20),  # overlay arena FixedHeap: +256 MiB
    0x034ED66C: (0x3C600FD0, 'botw_more_memory()', 0x3C601FD0),  # overlay arena MoveableMemoryHeap: +256 MiB
    0x034ED55C: (0x3C600040, 'botw_more_memory()', 0x3C600060),  # FixedHeap GameDataWork
    0x034ECED8: (0x3C600200, 'botw_more_memory()', 0x3C600600),  # FixedHeap TexArcWork: +64 MiB
    0x034ED1E4: (0x3CC001A0, 'botw_more_memory()', 0x3CC003A0),  # FixedHeap Audio: +32 MiB
    0x0340F31C: (0x38E00069, 'botw_more_memory()', 0x38E000A7),  # "Main" (MiB): li r7, 0x87 + 0x20
    0x0340F32C: (0x38E00087, 'botw_more_memory()', 0x38E000A7),
    0x0340F334: (0x392000FB, 'botw_more_memory()', 0x392001FB),  # MovableMemory (MiB): li r9, 0xFB + 0x100
    0x0340F33C: (0x39200137, 'botw_more_memory()', 0x392001FB),
    0x0340F3F4: (0x3C000940, 'botw_more_memory()', 0x3C001940),  # ForResourceS: +256 MiB
    0x0340F370: (0x3F5A0020, 'botw_more_memory()', 0x3F402200),  # ForResourceL: addis r26, r26, 0x20 -> lis r26, 0x2200
    0x037C0F30: (0x3B9C0005, 'botw_more_memory()', 0x3B80009A),  # PlacementMgr heap (MiB): li r28, 0x5A + 0x40
    0x02C4136C: (0x39401000, 'botw_more_memory()', 0x39401400),  # PlacementMgr pools: li r10, 0x1000 + 0x400
    0x02C413F0: (0x39401000, 'botw_more_memory()', 0x39401400),
    0x02C43E64: (0x39401000, 'botw_more_memory()', 0x39401400),
    0x02C46340: (0x39401000, 'botw_more_memory()', 0x39401400),
}
SITES.update(MEMORY_SITES)


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
    return '#include "botw_cheats.h"\n#include "botw_distance.h"\n#include "botw_memory.h"\n' + '\n'.join(lines)
