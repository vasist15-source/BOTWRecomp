"""v208 fixed-frame exceptions, applied only after the RPX identity check.

Sites were cross-checked against Cemu graphic packs' FPS++ v208 patches.
Use our engine's measured interval, not the requested FPS or bullet-time speed.
Original control flow, object properties, and unrelated registers stay intact.
"""

def single(register, expression):
    return (f'{{ double v = ppc_to_single(c, {expression}); '
            f'c->f[{register}].ps0 = v; c->f[{register}].ps1 = v; }}')


RATIO = 'botw_frame_ratio()'
# address: (original instruction, before/after/replace, emitted operation)
SITES = {
    0x02D5F760: (0xC02B00FC, 'after', single(1, f'c->f[1].ps0 * {RATIO}')),
    0x02D90D30: (0xC16A7964, 'replace', single(11, f'30.0 / {RATIO}')),
    0x02D90D8C: (0xC16A7964, 'replace', single(11, f'30.0 / {RATIO}')),
    0x02C6B8F8: (0xFC0DE800, 'before', single(13, f'c->f[13].ps0 / {RATIO}')),
    0x020E551C: (0xC16BCCAC, 'replace', single(11, f'30.0 * {RATIO}')),
    0x020E731C: (0xC1ACCCAC, 'replace', single(13, f'30.0 * {RATIO}')),
    0x020E3EB8: (0xC1ACCB18, 'replace', single(13, f'0.5 / {RATIO}')),
    0x020E5398: (0xC1AACB18, 'replace', single(13, f'0.5 / {RATIO}')),
    0x033609E0: (0xC0064F48, 'replace', single(0, RATIO)),
    0x02B951F0: (0xC1640000, 'replace', single(11, f'1.5 * {RATIO}')),
    0x0348BA3C: (0xEF3B6824, 'replace', single(25, '30.0')),
    0x022AC944: (0xEC0C002A, 'replace', single(0, f'c->f[12].ps0 + {RATIO}')),
    0x0209FFC0: (0xC00936B4, 'after', single(0, f'c->f[0].ps0 * {RATIO}')),
    0x0333D350: (0xC1990048, 'replace', single(12, '0.03333334f')),
    0x0333D80C: (0xED29FA7C, 'replace', 'c->f[9].ps0 = c->f[31].ps0;'),
    0x0333DC78: (0xED29F27C, 'replace', 'c->f[9].ps0 = c->f[30].ps0;'),
    0x0333DD58: (0xED29F27C, 'replace', 'c->f[9].ps0 = c->f[30].ps0;'),
    0x034A804C: (0xEFDE0072, 'replace',
        single(30, 'c->f[30].ps0 - 1.0') +
        single(30, 'c->f[30].ps0 * (double)0.45f') +
        single(30, 'c->f[30].ps0 + 1.0') +
        single(30, 'c->f[30].ps0 * round25(c->f[1].ps0)')),
    # FPS++'s Thunderblight workaround accelerates two movement timers. Keep
    # the shared data constant untouched and preserve the original 30-Hz path.
    0x022D0F30: (0xC3EC75AC, 'replace', single(31, f'{RATIO} < 1.0 ? -999.0 : -1.0')),
    0x022D1098: (0xC00C75AC, 'replace', single(0, f'{RATIO} < 1.0 ? -999.0 : -1.0')),
    # The real v208 instruction here loads the event name; keep it. The
    # upstream annotation incorrectly describes a different store instruction.
    0x031D2D2C: (0x819D0008, 'after',
        'botw_cutscene_begin(c->r[31], c->r[9], c->r[12]);'),
    0x031D3798: (0x817E027C, 'after',
        'botw_cutscene_end(c->r[30], ld32(c->r[30] + 0x274));'),
}


def apply(source, sites):
    if not sites:
        return source
    lines = source.splitlines()
    for address in sites:
        word, mode, operation = SITES[address]
        marker = f'/* {address:08X}: {word:08X} */'
        matches = [i for i, line in enumerate(lines) if marker in line]
        if len(matches) != 1:
            raise ValueError(f'Missing or ambiguous FPS instruction {address:08X}')
        index = matches[0]
        original = lines[index]
        body = {'before': operation + original, 'after': original + operation,
                'replace': operation + marker}[mode]
        lines[index] = (f'    /* BOTW_FPS_BEGIN {address:08X} */\n' + body +
                        f'\n    /* BOTW_FPS_END {address:08X} */')
    return ('#include "botw_fps.h"\n' + '\n'.join(lines))
