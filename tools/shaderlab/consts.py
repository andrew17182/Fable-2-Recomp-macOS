#!/usr/bin/python3
"""consts.py consts.txt PS [byte offsets...]: which system / fetch / bool-loop dwords are invariant across a PS's draws."""
import re, sys, struct, collections, json
rows = collections.defaultdict(list)
for l in open(sys.argv[1]):
    m = re.search(r'ps (\w+) draw (\d+) sys=(\w+) bl=(\w+) fc=(\w+)', l)
    if m: rows[m.group(1)].append({k: bytes.fromhex(m.group(i)) for k, i in (('sys', 3), ('bl', 4), ('fc', 5))})
ps = sys.argv[2]
R = rows[ps]
print(ps, 'draws', len(R))
out = {}
for k in ('sys', 'bl', 'fc'):
    n = len(R[0][k]) // 4
    vals = [struct.unpack(f'<{n}I', r[k]) for r in R]
    inv = {i * 4: vals[0][i] for i in range(n) if all(v[i] == vals[0][i] for v in vals)}
    var = [i * 4 for i in range(n) if i * 4 not in inv]
    out[k] = {'invariant': inv, 'varying': var}
    print(k, 'dwords', n, 'invariant', len(inv), 'varying offsets', var[:40])
json.dump({k: {'invariant': {str(o): v for o, v in d['invariant'].items()}, 'varying': d['varying']} for k, d in out.items()}, open(f'consts-{ps}.json', 'w'))
