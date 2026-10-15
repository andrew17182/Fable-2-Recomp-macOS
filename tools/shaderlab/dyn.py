#!/usr/bin/python3
"""Dynamic vs static instructions per shader: sum draw-level 'Instructions' over every draw of the frame whose
pipeline uses the PS, divided by that PS's SIMD-group count (from /performance/shaders). Usage: dyn.py SESSION PSHASH..."""
import json, re, subprocess, sys
S = sys.argv[1]; want = [h.upper() for h in sys.argv[2:]]
def gd(c): return subprocess.run(['gpudebug', '-s', S, '-c', c], capture_output=True, text=True).stdout
def gdj(c): return json.loads(subprocess.run(['gpudebug', '-s', S, '--json', '-c', c], capture_output=True, text=True).stdout)
rows = json.load(open('ranked-shaders.json'))
pso_of = {}
for r in rows:
    f = r.get('fragment', '')
    for h in want:
        if h in f: pso_of.setdefault(h, set()).add(r['pso'].split()[0][1:])
simd = {h: 0 for h in want}; static = {}
gd('go /performance/shaders')
for r in rows:
    f = r.get('fragment', '')
    for h in want:
        if h in f and r['name'].startswith('frag'):
            t = gd(f"info {r['name']}")
            simd[h] += int(re.search(r'SIMD groups\s+(\d+)', t).group(1))
            static.setdefault(h, []).append(int(re.search(r'^Instructions\s+(\d+)', t, re.M).group(1)))
gd('go /performance/commands')
cmds = gdj('list --all')['children']
dyn = {h: 0 for h in want}; nd = {h: 0 for h in want}
for c in cmds:
    v = [x.get('value') for x in c['values']]
    obj = next((x.split()[0][1:] for x in v if isinstance(x, str) and x.startswith('@rps')), None)
    for h in want:
        if obj in pso_of.get(h, ()):
            t = gd(f"info {c['name']}")
            m = re.search(r'^Instructions:\s+(\d+)', t, re.M)
            if m: dyn[h] += int(m.group(1)); nd[h] += 1
for h in want:
    print(f"PS {h}: draws {nd[h]} simd_groups {simd[h]} draw-instr {dyn[h]} -> {dyn[h]/max(1,simd[h]):.0f} instr/SIMD-group; static {static.get(h)}")
