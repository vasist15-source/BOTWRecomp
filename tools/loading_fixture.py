"""Extract actual loading-pane close/predicate functions; no game code in Git."""
import pathlib
import re
import sys

FUNCTIONS = {141: ('0307CC9C', '0307CD2C'), 225: ('03A3E880',)}


def extract(generated, output):
    bodies = []
    for unit, names in FUNCTIONS.items():
        text = (pathlib.Path(generated) / f'code_{unit:03d}.c').read_text()
        for name in names:
            body = re.search(r'void f_' + name + r'\(Cpu\* __restrict c\) \{.*?\n\}', text, re.S)[0]
            bodies.append(re.sub(r'    MUSTTAIL return f_[0-9A-F]+\(c\);\n(?=\})', '', body))
    declarations = '\n'.join(f'void f_{name}(Cpu*);' for names in FUNCTIONS.values() for name in names)
    pathlib.Path(output).write_text('#include "ppc.h"\n' + declarations + '\n' + '\n'.join(bodies))


if __name__ == '__main__':
    extract(*sys.argv[1:])
