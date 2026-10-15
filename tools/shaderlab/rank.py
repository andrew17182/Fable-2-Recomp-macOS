#!/usr/bin/python3
"""Map gpudebug /performance/shaders entries to guest VS/PS hashes (via pipeline info) and rank."""
import json, re, subprocess, sys, collections
S = sys.argv[1]
d = json.load(open('perf-shaders-all.json'))
cache = {}
def info(obj):
    if obj not in cache:
        cache[obj] = subprocess.run(['gpudebug', '-s', S, '-c', f'info @{obj}'], capture_output=True, text=True).stdout
    return cache[obj]
rows = []
for c in d['children']:
    v = [x['value'] for x in c['values']]
    fn, pso, cost, inv = v[0], v[1], float(v[2]), v[3]
    obj = pso.split()[0][1:]
    txt = info(obj)
    labs = dict(re.findall(r'^\s*(vertex|fragment|mesh|object|compute|kernel):\s+\S+ \(lib\d+ "([^"]*)"\)', txt, re.M))
    rows.append(dict(name=c['name'], fn=fn, pso=pso, cost=cost, inv=inv, **labs))
json.dump(rows, open('ranked-shaders.json', 'w'), indent=1)
by_ps = collections.defaultdict(float); by_vs = collections.defaultdict(float)
for r in rows:
    st = r['name'][:4]
    if st == 'frag': by_ps[r.get('fragment', '?')] += r['cost']
    if st in ('vert', 'mesh', 'obje'): by_vs[(st, r.get('vertex') or r.get('mesh') or r.get('object') or r['pso'])] += r['cost']
print('stage totals', {k: round(sum(r['cost'] for r in rows if r['name'].startswith(k)), 1) for k in ('frag', 'vert', 'mesh', 'obje', 'comp', 'tile')})
print('--- top PS'); [print(f'{c:6.2f}%  {k}') for k, c in sorted(by_ps.items(), key=lambda x: -x[1])[:25]]
print('--- top VS'); [print(f'{c:6.2f}%  {k}') for k, c in sorted(by_vs.items(), key=lambda x: -x[1])[:15]]
