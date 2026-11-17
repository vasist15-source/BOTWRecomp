"""v208 aspect ratio sites (Settings > Display > Aspect ratio), applied only after the RPX identity check.

The game draws a 1280x720 (16:9) TV picture. At another aspect ratio the renderer allocates the screen
targets wider (runtime/src/gfx/metal_surfaces.mm) and the game's code is told about the new shape at
these sites (runtime/src/aspect.cpp answers; at 16:9 every site returns the game's own value):

- 0386D020: the 3D camera's projection takes its aspect from its viewport (width / height of the
  viewport at r29 + 0x18..0x24), stored at projection + 0xB4 next. A 16:9 viewport gets the TV's
  aspect: Hor+ (more to the sides, same vertical field of view). Other shapes keep theirs.
- 0386CFE8: the same function hands the camera's vertical field of view (radians, camera + 0x68) to the
  projection's setFovy (030C16D0). For a 16:9 viewport and a picture narrower than 16:9 it is widened
  so the horizontal one stays (Vert+): nothing of the 16:9 view is cut off. Wider pictures keep it.
- 035B0A38: grass culling loads 1.0 (102E94D8) and divides it by the view's scale; wider pictures load
  (16/9) / aspect so grass reaches the new edges.
- 03C479F8: nw::lyt DrawInfo::LoadProjectionMtx (03C479D4) uploads the layout projection (16 floats at
  DrawInfo + 0) with GX2SetVertexUniformReg. The upload is tagged; the render thread narrows it when
  the target is the TV picture, so HUD, menus and text keep their proportions, centred.
- 0421A784: nw::font's text draw (0421A3A8, called by TextBox panes) uploads projection (content + 4)
  times view (content + 8), the same tag.

Read-only data the game reads where it needs the TV's aspect (camera set-ups; 101BF8E8 = 1.777,
1030A57C and 1036DD4C = 16/9) and a second grass distance (1047BFB8 = 120) are written by aspect.cpp
at run time, so this module only has the code sites. The sites and data are those of Cemu graphic
packs' BotW Graphics pack (patch_AspectRatio.asm, v208; graphic packs v989, CC0-1.0); the layout and
text projections replace its 2,900 lines of per-pane HUD rules with one centred HUD.
"""

def single(register, expression):
    return (f'{{ double v = ppc_to_single(c, {expression}); '
            f'c->f[{register}].ps0 = v; c->f[{register}].ps1 = v; }}')


# address: (original instruction, before/after/replace, emitted operation)
SITES = {
    0x0386CFE8: (0xC03F0068, 'after', single(1, 'botw_aspect_fovy(c->f[1].ps0, c->r[29])')),   # lfs f1, 0x68(r31)
    0x0386D020: (0xED8B6024, 'after', single(12, 'botw_aspect_view(c->f[12].ps0)')),       # fdivs f12, f11, f12
    0x035B0A38: (0xC16594D8, 'replace', single(11, 'botw_aspect_grass()')),                 # lfs f11, -0x6B28(r5)
    0x03C479F8: (0x48700CE1, 'before', 'botw_aspect_tag_projection();'),                   # bl GX2SetVertexUniformReg
    0x0421A784: (0x4812DF55, 'before', 'botw_aspect_tag_projection();'),                   # bl GX2SetVertexUniformReg
}


def apply(source, sites):
    """Patch the given sites of one function's C text (the function must be in plain form: c->r[N])."""
    if not sites:
        return source
    lines = source.splitlines()
    for address in sites:
        word, mode, operation = SITES[address]
        marker = f'/* {address:08X}: {word:08X} */'
        matches = [i for i, line in enumerate(lines) if marker in line]
        if len(matches) != 1:
            raise ValueError(f'Missing or ambiguous aspect instruction {address:08X}')
        index = matches[0]
        original = lines[index]
        body = {'before': '    ' + operation + '\n' + original, 'after': original + '\n    ' + operation,
                'replace': '    ' + operation + ' ' + marker}[mode]
        lines[index] = (f'    /* BOTW_ASPECT_BEGIN {address:08X} */\n' + body +
                        f'\n    /* BOTW_ASPECT_END {address:08X} */')
    return '#include "botw_aspect.h"\n' + '\n'.join(lines)
