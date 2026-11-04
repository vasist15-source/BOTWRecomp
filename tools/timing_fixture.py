"""Extract actual translated functions for timing regression; no guest code checked in."""
import pathlib
import re
import sys

FUNCTIONS = {132: ('02F62A6C',), 144: ('030BD28C', '030BD614', '030BD694'),
             205: ('037DC2F8',), 238: ('03BAAC64', '03BAAC9C')}


def extract(generated, output):
    bodies = []
    for unit, functions in FUNCTIONS.items():
        source = (pathlib.Path(generated) / f'code_{unit:03d}.c').read_text()
        for name in functions:
            body = re.search(r'void f_' + name + r'\(Cpu\* __restrict c\) \{.*?\n\}', source, re.S)[0]
            # Unreachable emitted fallthrough names need not link the whole game.
            body = re.sub(r'    MUSTTAIL return f_[0-9A-F]+\(c\);\n(?=\})', '', body)
            bodies.append(body)
    declarations = '\n'.join(f'void f_{name}(Cpu*);' for names in FUNCTIONS.values() for name in names)
    pathlib.Path(output).write_text('#include "ppc.h"\n' + declarations +
        '\nvoid imp_coreinit_OSGetCoreId(Cpu*);\nvoid f_03BB14C0(Cpu*);\nvoid f_030AB9A0(Cpu*);\n' +
        '\n'.join(f'void f_{name}(Cpu*);' for name in
                  ('02EA91F8', '02EA9E00', '02E90BFC', '02EAA15C', '02EA9FB8', '02EA9F7C', '02EA9B68')) +
        '\n' + '\n'.join(bodies))


if __name__ == '__main__':
    extract(*sys.argv[1:])
