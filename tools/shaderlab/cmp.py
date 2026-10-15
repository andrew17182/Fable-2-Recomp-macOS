#!/usr/bin/python3
"""cmp.py TAG [TAG...]: per-replay main-pass time and per-guest-PS absolute cost (ms of replay shader time),
using the pipeline -> label map from ranked-shaders.json (trace object ids are stable across replays)."""
import json, re, sys, collections
lab = {r['pso'].split()[0][1:]: r.get('fragment') or r.get('vertex') or r.get('mesh') or r['fn'] for r in json.load(open('ranked-shaders.json'))}
WATCH = sys.argv[sys.argv.index('--ps') + 1].split(',') if '--ps' in sys.argv else ['AF1E16EE', '0F99810F']
tags = [a for a in sys.argv[1:] if not a.startswith('--') and ',' not in a]
for t in tags:
    info = open(f'ab/{t}/re119-info.txt').read()
    dur = float(re.search(r'Duration:\s+([\d.]+)', info).group(1)); cost = float(re.search(r'^Cost:\s+([\d.]+)', info, re.M).group(1))
    total = dur / cost * 100
    agg = collections.defaultdict(float)
    for c in json.load(open(f'ab/{t}/shaders.json'))['children']:
        v = [x.get('value') for x in c['values']]
        pso = v[1].split()[0][1:]
        l = lab.get(pso, v[1])
        for w in WATCH:
            if w in l: agg[w] += v[2] * total / 100
    fr = float(re.search(r'Fragment:\s+([\d.]+)', info).group(1))
    print(f'{t:8s} main pass {dur:.3f} ms (frag {fr/cost*dur:.3f}) | shader total {total:.2f} ms | ' + '  '.join(f'{w} {agg[w]:.3f} ms' for w in WATCH))
