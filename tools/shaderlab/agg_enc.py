#!/usr/bin/python3
"""Aggregate an encoder's draw costs (gpudebug commands list JSON) by pipeline -> guest PS/VS label."""
import json, re, subprocess, sys, collections
S, fn = sys.argv[1], sys.argv[2]
d = json.load(open(fn))
cache = {}
def labs(obj):
    if obj not in cache:
        t = subprocess.run(['gpudebug', '-s', S, '-c', f'info @{obj}'], capture_output=True, text=True).stdout
        cache[obj] = dict(re.findall(r'^\s*(vertex|fragment|mesh|object):\s+\S+ \(lib\d+ "([^"]*)"\)', t, re.M))
    return cache[obj]
agg = collections.defaultdict(lambda: [0.0, 0]); tot = 0
for c in d['children']:
    v = [x.get('value') for x in c['values']]
    if len(v) < 3 or not isinstance(v[2], float): continue
    obj = v[1].split()[0][1:]
    l = labs(obj)
    key = (l.get('fragment', '-'), l.get('vertex') or ('geom-emu ' + l.get('mesh', '?')))
    agg[key][0] += v[2]; agg[key][1] += 1; tot += v[2]
print(f'total cost {tot:.2f}% over {len(d["children"])} commands')
for k, (c, n) in sorted(agg.items(), key=lambda x: -x[1][0])[:25]:
    print(f'{c:6.2f}%  draws {n:4d}  {k[0]:45s} {k[1]}')
