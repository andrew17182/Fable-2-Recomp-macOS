#!/usr/bin/python3
"""For each dumped PS metallib: is it plain (metal-opt -O2 shrinks the bitcode) or already optimized?"""
import os, subprocess, sys, struct, json
sys.path.insert(0, os.path.dirname(__file__))
TC = os.path.dirname(subprocess.run(['/usr/bin/xcrun', '--find', 'metal-opt'], capture_output=True, text=True).stdout.strip())
res = {}
os.makedirs('air/all', exist_ok=True)
for f in sorted(os.listdir('nsdump')):
    if not f.startswith('ps_'): continue
    b = open('nsdump/' + f, 'rb').read(); i = b.find(bytes.fromhex('dec0170b'))
    off, size = struct.unpack('<II', b[i + 8:i + 16]); bc = 'air/all/' + f[:-9] + '.bc'
    open(bc, 'wb').write(b[i:i + off + size])
    a = subprocess.run([TC + '/metal-opt', '-S', bc, '-o', '-'], capture_output=True, text=True).stdout.count('\n')
    o = subprocess.run([TC + '/metal-opt', '-O2', '-S', bc, '-o', '-'], capture_output=True, text=True).stdout.count('\n')
    res[f[3:-9]] = dict(lines=a, o2_lines=o, plain=o < a * 0.97)
json.dump(res, open('plaincheck.json', 'w'), indent=1)
print('plain', sum(r['plain'] for r in res.values()), 'of', len(res))
