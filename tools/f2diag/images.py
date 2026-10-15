"""Capture analysis without PIL or numpy (P17): PPM reading, artifact detectors, movement check.

Moved from out/validation: flash-20261007/dark.py (transient dark-polygon dips), playtest-20261005 and
flash-20261007/analyze.py + blue.py (blue/white flashes), flash-20261007/movecheck.py (did the hero move),
area-load-20261008/run.py stats() (mean luma + changed fraction). The detectors work on a 320x180
grid sampled from each capture; numpy is used when it is importable, otherwise pure Python.
"""
import subprocess
from pathlib import Path

GRID_W, GRID_H = 320, 180


def read_ppm(path):
    """(width, height, rgb bytes)"""
    data = Path(path).read_bytes()
    header, pixels = data.split(b'\n255\n', 1)
    w, h = map(int, header.split()[1:3])
    return w, h, pixels


def grid(path):
    """320x180 list of (r, g, b): the mean of each cell's 4 corner-ish samples (stride sampling)."""
    w, h, px = read_ppm(path)
    sx, sy = max(1, w // GRID_W), max(1, h // GRID_H)
    out = []
    for gy in range(GRID_H):
        y0 = min(h - 1, gy * sy)
        y1 = min(h - 1, y0 + sy // 2)
        r0, r1 = y0 * w * 3, y1 * w * 3
        for gx in range(GRID_W):
            x0 = min(w - 1, gx * sx) * 3
            x1 = min(w - 1, gx * sx + sx // 2) * 3
            r = (px[r0 + x0] + px[r0 + x1] + px[r1 + x0] + px[r1 + x1]) >> 2
            g = (px[r0 + x0 + 1] + px[r0 + x1 + 1] + px[r1 + x0 + 1] + px[r1 + x1 + 1]) >> 2
            b = (px[r0 + x0 + 2] + px[r0 + x1 + 2] + px[r1 + x0 + 2] + px[r1 + x1 + 2]) >> 2
            out.append((r, g, b))
    return out


def luma(cells):
    return [(r + g + b) / 3.0 for r, g, b in cells]


def stats(path, prev=None):
    """(mean luma, changed fraction vs prev luma grid or None, luma grid) like area-load run.py."""
    L = luma(grid(path))
    mean = sum(L) / len(L)
    diff = None if prev is None or len(prev) != len(L) else sum(abs(a - b) > 12 for a, b in zip(L, prev)) / len(L)
    return round(mean, 1), (None if diff is None else round(diff, 3)), L


def dedupe(frames):
    """Captures with distinct frame ids, in order (sweeps capture faster than the game swaps)."""
    seen, out = set(), []
    for m in frames:
        if m.get('frame_id') in seen:
            continue
        seen.add(m.get('frame_id'))
        out.append(m)
    return out


def dark_dips(frames, thresh=50.0, flag=0.002):
    """dark.py: cells darker than BOTH neighbours by > thresh while the neighbours agree.
    frames: capture metadata dicts with 'path'. Returns rows; 'flagged' when dark fraction > flag."""
    fr = dedupe(frames)
    L = [luma(grid(m['path'])) for m in fr]
    rows = []
    for i in range(1, len(fr) - 1):
        a, b, c = L[i - 1], L[i], L[i + 1]
        hits = [k for k in range(len(b)) if min(a[k], c[k]) - b[k] > thresh and abs(a[k] - c[k]) < thresh * 0.6]
        xs = [k % GRID_W for k in hits]
        ys = [k // GRID_W for k in hits]
        dark = len(hits) / len(b)
        rows.append(dict(i=i, frame=fr[i].get('frame_id'), phase=fr[i].get('phase'), dark=round(dark, 5),
                         flagged=dark > flag, bbox=[min(xs), min(ys), max(xs), max(ys)] if hits else None))
    return rows


def blue_spikes(frames, flag=0.008):
    """analyze.py/blue.py: blue (b>140, b>r+40, b>g+15) and white (all >220) pixel fractions, transient
    versions (not present in either neighbour) and luminance spikes; 'flagged' when blue > flag."""
    fr = dedupe(frames)
    G = [grid(m['path']) for m in fr]
    masks = []
    for cells in G:
        masks.append(([b > 140 and b > r + 40 and b > g + 15 for r, g, b in cells],
                      [r > 220 and g > 220 and b > 220 for r, g, b in cells]))
    rows = []
    for i, m in enumerate(fr):
        bl, wh = masks[i]
        n = len(bl)
        row = dict(i=i, frame=m.get('frame_id'), phase=m.get('phase'), blue=round(sum(bl) / n, 5), white=round(sum(wh) / n, 5))
        if 0 < i < len(fr) - 1:
            for name, idx in (('blue', 0), ('white', 1)):
                cur, p, q = masks[i][idx], masks[i - 1][idx], masks[i + 1][idx]
                row[name + '_transient'] = round(sum(c and not a and not b for c, a, b in zip(cur, p, q)) / n, 5)
            L, Lp, Ln = luma(G[i]), luma(G[i - 1]), luma(G[i + 1])
            row['lum_spike'] = round(sum(l - max(a, b) > 60 for l, a, b in zip(L, Lp, Ln)) / n, 5)
        row['flagged'] = row['blue'] > flag
        rows.append(row)
    return rows


def moved(before, after, thresh=12, min_fraction=0.05):
    """movecheck.py: True when more than min_fraction of the grid changed between two captures."""
    a, b = luma(grid(before)), luma(grid(after))
    frac = sum(abs(x - y) > thresh for x, y in zip(a, b)) / len(a)
    return frac > min_fraction, round(frac, 3)


def to_png(ppm, png=None):
    """Convert with the system's sips (no PIL)."""
    png = Path(png) if png else Path(ppm).with_suffix('.png')
    subprocess.run(['sips', '-s', 'format', 'png', str(ppm), '--out', str(png)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return png
