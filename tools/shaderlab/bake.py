#!/usr/bin/python3
"""bake.py in.ll consts-<PS>.json out.ll [--cb SYS=CB1,FC=CB2,BL=CB4] [--only fc,sys,bl]
Replace loads of invariant system / fetch / bool-loop constant dwords with literals (exact for draws whose
live values equal the logged ones; a runtime guard would compare them). Prints what it baked."""
import json, re, sys, struct
src, cj, dst = sys.argv[1:4]
cbmap = dict(kv.split('=') for kv in (sys.argv[sys.argv.index('--cb') + 1] if '--cb' in sys.argv else 'sys=CB1,fc=CB2,bl=CB4').split(','))
only = (sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else 'sys,fc,bl').split(',')
C = json.load(open(cj))
kind_of = {v: k for k, v in cbmap.items()}
s = open(src).read()
ptr = {}  # name -> (kind, byte offset)
for m in re.finditer(r'(%[\w.]+) = load i\d+ addrspace\(2\)\*, i\d+ addrspace\(2\)\* addrspace\(2\)\* %[\w.]+', s):
    pass
for cb in cbmap.values():
    base = f'%{cb}..unpack'
    for m in re.finditer(rf'(%[\w.]+) = (?:load [^\n]*?|)', ''):
        pass
names = {}
for m in re.finditer(r'(%(CB\d)\.\.unpack\d*) = load', s):
    names[m.group(1)] = m.group(2)
for n, cb in names.items():
    if cb in kind_of: ptr[n] = (kind_of[cb], 0)
changed = True
while changed:
    changed = False
    for m in re.finditer(r'(%[\w.]+) = getelementptr (?:inbounds )?(\w+), \w+ addrspace\(2\)\* (%[\w.]+), i64 (\d+)', s):
        if m.group(3) in ptr and m.group(1) not in ptr:
            k, o = ptr[m.group(3)]; scale = {'i8': 1, 'i32': 4, 'float': 4}[m.group(2)]
            ptr[m.group(1)] = (k, o + int(m.group(4)) * scale); changed = True
    for m in re.finditer(r'(%[\w.]+) = bitcast \w+ addrspace\(2\)\* (%[\w.]+) to', s):
        if m.group(2) in ptr and m.group(1) not in ptr:
            ptr[m.group(1)] = ptr[m.group(2)]; changed = True
baked, skipped = [], []
def rep(m):
    name, ty, p = m.group(1), m.group(2), m.group(3)
    if p not in ptr: return m.group(0)
    k, o = ptr[p]
    inv = C[k]['invariant']
    if k not in only or str(o) not in inv:
        skipped.append((k, o)); return m.group(0)
    v = inv[str(o)]
    baked.append((k, o, v))
    if ty == 'i32': return f'{name} = or i32 {v}, 0'
    if ty == 'float': return f'{name} = bitcast i32 {v} to float'
    skipped.append((k, o, ty)); return m.group(0)
s2 = re.sub(r'(%[\w.]+) = load (i32|float), (?:i32|float) addrspace\(2\)\* (%[\w.]+), align \d+', rep, s)
open(dst, 'w').write(s2)
print('baked', len(baked), sorted(set((k, o) for k, o, v in baked)))
print('kept dynamic (varying)', sorted(set(x[:2] for x in skipped)))
