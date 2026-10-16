#!/usr/bin/env python3
"""Pair the terrain patches of each dumped frame across passes (docs/adaptive-terrain-flashes-20261014.md).

  python3 tools/tess_frame_match.py <dump.jsonl> [--verbose]

Input: FABLE2_TESS_FRAME_DUMP lines (metal_command_processor.cc), one per tessellated draw.
Ground key of a patch, from the guest domain shaders' ucode (as MatchTerrainPatchFactors computes it):
- adaptive terrain 5003700B: idx = patch + VGT_INDX_OFFSET + c113.x; row = floor(idx * c11.y);
  col = idx - row * c11.x; origin = (col, row) * c8.xy + c47.xy, size c8.x;
- continuous layers C30A97D9 / FB68A7F2: idx = index (or draw position) + VGT_INDX_OFFSET; same row/col;
  origin = (col, row) * c46.xy + c113.xy, size c46.x;
- plus the depth target (RB_DEPTH_INFO) and the heightmap (tf16) address.
For every layer patch: does it have the factors and the path of the opaque patch under it (the last opaque
draw of that frame over the same ground)? Also: neighbour edge agreement of the guest's adaptive factors.
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import collections
import json
import math
import re

OPAQUE = '5003700B7C9B1C16'
LAYERS = ('C30A97D946FA2BE4', 'FB68A7F2301210E1')


def load(path):
    for line in open(path):
        # fmt writes nan / inf for unset constants.
        line = re.sub(r'(?<=[,\[:])-?(nan|inf)', 'null', line)
        try:
            yield json.loads(line)
        except ValueError:
            continue


def key(d, idx, add, size, off):
    c = d['c']
    idx = min(max((idx + d['indx_offset']) & 0xFFFFFF, d['min_index']), d['max_index'])
    x = float(idx) + add
    row = math.floor(x * c['11'][1])
    col = x - row * c['11'][0]
    return (d['depth_info'], int(d['tf']['16'], 16) & 0xFFFFF000,
            col * c[size][0] + c[off][0], row * c[size][1] + c[off][1], c[size][0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    tot = collections.Counter()
    edges = collections.Counter()
    examples = []
    frame, opaque = None, {}

    def edge_check():
        # cp1 of a patch = cp3 of its +x neighbour, cp2 = cp0 of its +y neighbour (guest data, crack-free).
        for (dk, tf, x, y, s), (f, _) in opaque.items():
            r = opaque.get((dk, tf, x + s, y, s))
            u = opaque.get((dk, tf, x, y + s, s))
            if r:
                edges['x_pairs'] += 1
                edges['x_agree'] += f[1] == r[0][3]
            if u:
                edges['y_pairs'] += 1
                edges['y_agree'] += f[2] == u[0][0]

    for d in load(a.dump):
        if d['frame'] != frame:
            edge_check()
            frame, opaque = d['frame'], {}
        fac = d.get('factors') or []
        if d['vs'] == OPAQUE and d['mode'] == 2:
            for p in range(len(fac) // 4):
                opaque[key(d, p, d['c']['113'][0], '8', '47')] = (tuple(fac[p * 4:p * 4 + 4]), d['path'])
        elif d['vs'] in LAYERS and d['mode'] == 1:
            idxs = d.get('indices', range(d['count']))
            for p, v in enumerate(idxs):
                tot['layer_patches'] += 1
                o = opaque.get(key(d, v, 0.0, '46', '113'))
                if o is None:
                    tot['unpaired'] += 1
                    continue
                lf = tuple(fac[p * 4:p * 4 + 4]) if len(fac) >= p * 4 + 4 else (d['fmax'],) * 4
                same_f = lf == o[0]
                same_path = d['path'] == o[1]
                tot['opaque_not_max'] += o[0] != (d['fmax'],) * 4
                tot['same_factors'] += same_f
                tot['same_path'] += same_path
                tot['same_both'] += same_f and same_path
                if not (same_f and same_path) and len(examples) < 10:
                    examples.append((d['frame'], d['draw'], d['vs'][:8], d['ps'][:8], lf, d['path'], o))
    edge_check()
    print('layer patches %(layer_patches)d, unpaired %(unpaired)d' % collections.defaultdict(int, tot))
    paired = tot['layer_patches'] - tot['unpaired']
    if paired:
        for k in ('opaque_not_max', 'same_factors', 'same_path', 'same_both'):
            print('  %-15s %7d  (%.2f%% of paired)' % (k, tot[k], 100.0 * tot[k] / paired))
    if edges['x_pairs']:
        print('guest factors: +x edge agreement %.2f%% of %d, +y %.2f%% of %d' % (
            100.0 * edges['x_agree'] / edges['x_pairs'], edges['x_pairs'],
            100.0 * edges['y_agree'] / max(1, edges['y_pairs']), edges['y_pairs']))
    if a.verbose:
        for e in examples:
            print('  differs:', e)


if __name__ == '__main__':
    main()
