#!/usr/bin/env python3
"""Generate entry-trace wrappers (CMake option TB2_FN_TRACE): gen_fn_trace.py <generated_dir> <out.cpp> <hooked.cpp> lo-hi
Skips functions already overridden in <hooked.cpp>. Wrapper calls Tb2FnTrace(addr, lr) then the original."""
import glob, re, sys
gen_dir, out_path, hooked, span = sys.argv[1:5]
lo, hi = (int(x, 16) for x in span.split('-'))
import os
skip = set()
for f in glob.glob(os.path.join(os.path.dirname(hooked), '*.cpp')):
    skip |= set(int(m, 16) for m in re.findall(r'[(, ](?:sub_)?([0-9A-F]{8})[),]', open(f, errors='replace').read()))
funcs = set()
for p in glob.glob(f'{gen_dir}/*_recomp.*.cpp'):
    for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', open(p, errors='replace').read(), re.M):
        a = int(m.group(1), 16)
        if lo <= a < hi and a not in skip: funcs.add(a)
out = ['#include <cstdint>', '#include <rex/ppc/context.h>', '#include <rex/ppc/func.h>', 'void Tb2FnTrace(uint32_t addr, uint32_t lr);']
for a in sorted(funcs):
    out.append(f'extern "C" REX_FUNC(__imp__sub_{a:08X});\nextern "C" REX_FUNC(sub_{a:08X}) {{ Tb2FnTrace(0x{a:08X}, (uint32_t)ctx.lr); __imp__sub_{a:08X}(ctx, base); }}')
open(out_path, 'w').write('\n'.join(out) + '\n')
print(len(funcs), 'wrappers')
