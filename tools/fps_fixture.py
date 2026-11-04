"""Extract real translated FPS sites and the full bow-drain caller/callee.

This is not a second implementation of game calculations. Unrelated actor
services are supplied by the harness; no game code is stored in the repository.
"""
import pathlib
import re
import sys
from botw_fps_patches import SITES


def extract(generated, output):
    bodies, found = [], set()
    full = {'02D5F600', '02D90A54', '037924B4'}
    for path in sorted(pathlib.Path(generated).glob('code_*.c')):
        if int(path.stem.split('_')[1]) not in {5,7,23,24,100,108,116,117,153,165,166,177,202}:
            continue
        source = path.read_text()
        for match in re.finditer(r'void f_([0-9A-F]+)\(Cpu\* __restrict c\) \{.*?\n\}', source, re.S):
            if match[1] in full:
                body = re.sub(r'    MUSTTAIL return f_[0-9A-F]+\(c\);\n(?=\})', '', match[0])
                bodies.append(body)
        for address in SITES:
            marker=f'{address:08X}'
            pattern=r'/\* BOTW_FPS_BEGIN '+marker+r' \*/(.*?)/\* BOTW_FPS_END '+marker+r' \*/'
            patched=re.search(pattern, source, re.S)
            original=re.search(r'^.*?/\* '+marker+r': [0-9A-F]+ \*/$', source, re.M)
            if patched or original:
                if address in found: raise ValueError('Duplicate site '+marker)
                found.add(address)
                body=patched[1] if patched else original[0]
                bodies.append(f'void site_{marker}(Cpu* c) {{\n{body}\n}}')
    if found!=set(SITES): raise ValueError('Missing FPS sites')
    calls=set(re.findall(r'\b(f_[0-9A-F]+)\(c\)', '\n'.join(bodies)))
    declarations='\n'.join(f'void {name}(Cpu*);' for name in sorted(calls))
    pathlib.Path(output).write_text('#include "ppc.h"\n#include "botw_fps.h"\nvoid imp_coreinit_OSGetCoreId(Cpu*);\n'+declarations+'\n'+'\n'.join(bodies))


if __name__=='__main__': extract(*sys.argv[1:])
