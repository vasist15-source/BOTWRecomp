"""Extract the actual v208 JPEG codec and nonlocal-return routines for offline tests.

Generated game code/data stays in the build directory, never in this repository.
"""
import pathlib
import re
import sys


def extract(generated, output):
    functions = {}
    for unit in (244, 245, 246, 293):
        text = (pathlib.Path(generated) / f'code_{unit:03d}.c').read_text()
        for name, body in re.findall(r'void (f_[0-9A-F]+)(\(Cpu\* __restrict c\) \{.*?\n\})', text, re.S):
            functions[name] = re.sub(r'    MUSTTAIL return f_[0-9A-F]+\(c\);\n(?=\})', '', 'void ' + name + body)
    selected = {name: body for name, body in functions.items()
                if 0x03C709FC <= int(name[2:], 16) < 0x03C8FE60 or
                name in ('f_0420EA7C', 'f_0420EBB4', 'f_04211A38', 'f_04211A40')}
    while True:
        refs = set(re.findall(r'\b(f_[0-9A-F]+|imp_\w+)\(c\)', '\n'.join(selected.values())))
        missing = refs - selected.keys()
        added = {name: functions[name] for name in missing if name in functions}
        if not added:
            break
        selected.update(added)
    assert all(name.startswith('imp_') for name in missing), missing
    declarations = '\n'.join(f'void {name}(Cpu*);' for name in sorted(selected.keys() | missing))
    dispatch = '\n'.join(f'case 0x{name[2:]}: {name}(c); return;' for name in sorted(selected))
    pathlib.Path(output).write_text('#include "botw_jump.h"\n#include <stdlib.h>\n' + declarations +
        '\n' + '\n'.join(selected.values()) + '\nvoid ppc_dispatch(Cpu* c) { switch(c->pc) {\n' +
        dispatch + '\ndefault: abort();\n}}\n')


if __name__ == '__main__':
    extract(*sys.argv[1:])
