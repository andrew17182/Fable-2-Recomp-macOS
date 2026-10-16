#!/usr/bin/env python3
"""Skip-intro stand-in movies for a source checkout (or any game data root).

  python3 tools/fable2_skip_intro.py ensure [--root DIR]   make <root>/out/skipintro/{ms,lh,mw}.bik if missing
  python3 tools/fable2_skip_intro.py status [--root DIR]

The game (src/core/fable2_skip_intro.h) redirects the logo movies to game:\\out\\skipintro\\*.bik when
FABLE2_SKIP_INTRO_MOVIES=1; a missing stand-in silently plays the full movie. The release/portable app makes them
with FableIIPrepare --stage; a source checkout had no step that made them (they came from an old validation script),
so on a fresh clone "Skip intro movies" still played the intros (2026-10-15). fable2_dev_session.py launch, the
playtest update and the game build call ensure(). Same trim as tools/macos/Prepare/main.swift trimBink: the first 2
frames of each Bink 1 file, header, frame index and sizes rewritten. Runs under /usr/bin/python3 (3.9)."""
import argparse
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MOVIES = (('microsoft_logo.bik', 'ms.bik'), ('lionhead_logo.bik', 'lh.bik'), ('middlewarelogos.bik', 'mw.bik'))
FRAMES = 2


def trim_bink(d, n=FRAMES):
    """Keep the first `n` frames of a Bink 1 file (port of Prepare's trimBink)."""
    if len(d) < 0x2C or d[:3] != b'BIK' or n < 1: raise ValueError('not a Bink movie this tool can trim')
    u32 = lambda at: struct.unpack_from('<I', d, at)[0]
    frames, tracks = u32(8), u32(0x28)
    header_end = 0x2C + tracks * 12
    if n > frames or header_end + (frames + 1) * 4 > len(d): raise ValueError('not a Bink movie this tool can trim')
    index = [u32(header_end + 4 * i) for i in range(frames + 1)]
    offsets = [v & ~1 for v in index]
    new_table_end = header_end + (n + 1) * 4
    delta = (header_end + (frames + 1) * 4) - new_table_end
    new_index = [((index[i] & ~1) - delta) | (index[i] & 1) for i in range(n)] + [offsets[n] - delta]
    if not offsets[0] <= offsets[n] <= len(d): raise ValueError('not a Bink movie this tool can trim')
    body = d[offsets[0]:offsets[n]]
    largest = max(offsets[i + 1] - offsets[i] for i in range(n))
    out = bytearray(d[:header_end])
    total = new_table_end + len(body)
    struct.pack_into('<IIII', out, 4, total - 8, n, largest, n)
    out += struct.pack('<%dI' % len(new_index), *new_index) + body
    if len(out) != total: raise ValueError('Bink trim size mismatch')
    return bytes(out)


def status(root=ROOT):
    out = Path(root) / 'out/skipintro'
    return dict(folder=str(out), present=[s for _, s in MOVIES if (out / s).is_file()],
                missing=[s for _, s in MOVIES if not (out / s).is_file()])


def ensure(root=ROOT, log=None):
    """Makes the missing stand-ins from <root>/data/art/videos. Never raises: returns a summary dict."""
    root = Path(root)
    videos, out = root / 'data/art/videos', root / 'out/skipintro'
    try:
        listing = {p.name.lower(): p for p in videos.iterdir()} if videos.is_dir() else {}
    except OSError:
        listing = {}
    made, missing = [], []
    for movie, short in MOVIES:
        dest = out / short
        if dest.is_file(): continue
        source = listing.get(movie)
        if source is None:
            missing.append(movie)
            continue
        try:
            data = trim_bink(source.read_bytes())
            out.mkdir(parents=True, exist_ok=True)
            tmp = dest.with_name(dest.name + '.tmp%d' % os.getpid())
            tmp.write_bytes(data)
            os.replace(str(tmp), str(dest))
            made.append(short)
        except (OSError, ValueError) as error:
            missing.append('%s (%s)' % (movie, error))
    result = dict(status(root), made=made, sources_missing=missing)
    if log and (made or missing):
        log('[skip-intro] stand-ins in %s: made %s%s' % (out, ', '.join(made) or 'none',
                                                         '; missing ' + ', '.join(missing) if missing else ''))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('cmd', choices=['ensure', 'status'])
    p.add_argument('--root', default=str(ROOT))
    a = p.parse_args()
    result = ensure(a.root, log=lambda t: print(t, file=sys.stderr)) if a.cmd == 'ensure' else status(a.root)
    print(json.dumps(dict(ok=True, result=result), indent=2))


if __name__ == '__main__':
    main()
