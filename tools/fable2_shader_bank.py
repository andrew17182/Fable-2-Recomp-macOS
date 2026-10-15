#!/usr/bin/env python3
"""Offline shader scan of the game's shader banks (docs/offline-shader-seed.md).

  python3 tools/fable2_shader_bank.py scan [--data DIR] [--xsh FILE]       counts; with --xsh: coverage of that list
  python3 tools/fable2_shader_bank.py recipe --xsh FILE [--xpso FILE] --out RECIPE.json [--data DIR]
  python3 tools/fable2_shader_bank.py rebuild RECIPE.json --cache CACHE_DIR [--data DIR]

Fable II keeps its shaders in data/Shaders/ShadersRelease.sbk (and the debug twin Shaders.sbk): a "ShaderBankFile"
with a constant-name table, then 147 zlib-compressed entries, each holding D3D shader containers (magic 0x102A1101)
with the Xenos microcode. The runtime shader list (cache/shaders/shareable/4D5307F1.xsh) stores the microcode the
GPU actually saw:
  - pixel shaders: byte-identical slices of a bank entry;
  - vertex shaders: a bank slice whose vertex-fetch instructions D3D patched at bind time for the vertex
    declaration (the remaining dwords are identical).
So a "recipe" (per shader: hash, type, bank entry + byte offset + length, the patched dword values and a sha256 of
the result) plus the pipeline list (.xpso: shader hashes, modifications and render state) rebuilds the exact .xsh
from a player's own disc, without shipping microcode. `rebuild` writes the .xsh and .xpso into CACHE_DIR/shaders/
shareable; the game's startup warm-up (or a headless FABLE2_SHADER_PREP run) then translates and compiles them.

Everything this tool reads or writes is game-derived: keep its output in ignored out/ (whether a recipe may ship
is the maintainer's release decision, docs/offline-shader-seed.md). Runs under /usr/bin/python3 (3.9).
"""
import argparse
import base64
import collections
import hashlib
import json
import struct
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TITLE = '4D5307F1'
BANKS = ('ShadersRelease.sbk', 'Shaders.sbk')  # the game loads the release bank; the debug twin is a fallback source
XSH_MAGIC, XSH_VERSION = 0x48534558, 0x20201219
CONTAINER_MAGICS = (b'\x10\x2a\x11\x00', b'\x10\x2a\x11\x01')
RECIPE_VERSION = 1
# A vertex shader is accepted as "bank slice + patches" only when at most this share of its dwords differ: D3D's
# bind-time patching touches the vertex-fetch instructions only. Beyond it the match is not trusted (no recipe).
MAX_PATCH_SHARE = 0.4


def data_root(arg=None):
    for cand in ([Path(arg)] if arg else []) + [ROOT, ROOT.parent / 'Fable-2-Recomp-macOS']:
        if (cand / 'data/Shaders').is_dir(): return cand
    raise RuntimeError('no data/Shaders under %s (pass --data GAME_DATA_ROOT)' % (arg or ROOT))


def read_bank(path):
    """[(entry bytes)] of one .sbk: the size table, the offset table, then per entry (u32 ?, u32 compressed size,
    zlib stream). The tables sit right before the first stream; their count is stored before the size table."""
    d = Path(path).read_bytes()
    if not d.startswith(b'ShaderBankFile'): raise ValueError('%s is not a ShaderBankFile' % path)
    first = d.find(b'\x78\xda')
    while first >= 0:
        for n in range(1, 4096):
            c = first - 8
            b = c - 4 * n
            a = b - 4 * n - 4
            if a < 16: break
            if struct.unpack_from('>I', d, a)[0] == n and struct.unpack_from('>I', d, b)[0] == 0:
                sizes = struct.unpack_from('>%dI' % n, d, a + 4)
                offsets = struct.unpack_from('>%dI' % n, d, b)
                entries = []
                for i in range(n):
                    s = c + offsets[i]
                    _, csize = struct.unpack_from('>II', d, s)
                    out = zlib.decompressobj().decompress(d[s + 8:s + 8 + csize])
                    if len(out) != sizes[i]: raise ValueError('%s entry %d: %d bytes, table says %d' % (path, i, len(out), sizes[i]))
                    entries.append(out)
                return entries
        first = d.find(b'\x78\xda', first + 1)
    raise ValueError('%s: no entry table found' % path)


def load_banks(root):
    banks = collections.OrderedDict()
    for name in BANKS:
        path = Path(root) / 'data/Shaders' / name
        if path.is_file(): banks[name] = read_bank(path)
    if not banks: raise RuntimeError('no shader bank in %s/data/Shaders' % root)
    return banks


def read_xsh(path):
    """[(hash, type 0=vertex 1=pixel, ucode bytes guest-endian)] of a shader storage file (shader_storage.h)."""
    x = Path(path).read_bytes()
    magic, version = struct.unpack_from('<II', x, 0)
    if magic != XSH_MAGIC or int.from_bytes(struct.pack('<I', version), 'big') != XSH_VERSION:
        raise ValueError('%s is not a shader storage file of this version' % path)
    out, at = [], 8
    while at + 12 <= len(x):
        h, w = struct.unpack_from('<QI', x, at)
        n = w & 0x7fffffff
        if at + 12 + 4 * n > len(x): break
        out.append((h, w >> 31, x[at + 12:at + 12 + 4 * n]))
        at += 12 + 4 * n
    return out


def count_containers(banks):
    return sum(e.count(m) for entries in banks.values() for e in entries for m in CONTAINER_MAGICS)


class Index:
    """Exact and patched lookups of microcode in the bank entries."""

    def __init__(self, banks):
        self.banks = banks
        self.triples = collections.defaultdict(list)
        for name, entries in banks.items():
            for i, e in enumerate(entries):
                for p in range(0, len(e) - 12, 4):
                    self.triples[e[p:p + 12]].append((name, i, p))

    def exact(self, ucode):
        for name, entries in self.banks.items():
            for i, e in enumerate(entries):
                p = e.find(ucode)
                while p >= 0 and p % 4:
                    p = e.find(ucode, p + 1)
                if p >= 0: return name, i, p
        return None

    def patched(self, ucode):
        """(bank, entry, offset, [(dword index, value)]) of the best-aligned slice, voted by shared 3-dword runs."""
        n = len(ucode) // 4
        votes = collections.Counter()
        for k in range(max(0, n - 2)):
            for name, i, p in self.triples.get(ucode[4 * k:4 * k + 12], ()):
                votes[(name, i, p - 4 * k)] += 1
        for (name, i, base), _ in votes.most_common(4):
            e = self.banks[name][i]
            if base < 0 or base + len(ucode) > len(e): continue
            src = e[base:base + len(ucode)]
            patches = [(k, struct.unpack_from('>I', ucode, 4 * k)[0]) for k in range(n) if src[4 * k:4 * k + 4] != ucode[4 * k:4 * k + 4]]
            if len(patches) <= MAX_PATCH_SHARE * n: return name, i, base, patches
        return None


def recipe(xsh, xpso=None, root=None, log=print):
    banks = load_banks(data_root(root))
    index = Index(banks)
    shaders, unresolved = [], []
    stats = collections.Counter()
    for h, kind, ucode in read_xsh(xsh):
        item = dict(hash='%016X' % h, type='pixel' if kind else 'vertex', dwords=len(ucode) // 4,
                    sha256=hashlib.sha256(ucode).hexdigest())
        found = index.exact(ucode)
        if found:
            item.update(bank=found[0], entry=found[1], offset=found[2], patches=[])
            stats[item['type'] + '_exact'] += 1
        else:
            found = index.patched(ucode)
            if not found:
                unresolved.append(item['hash'])
                stats[item['type'] + '_unresolved'] += 1
                continue
            item.update(bank=found[0], entry=found[1], offset=found[2], patches=[[k, v] for k, v in found[3]])
            stats[item['type'] + '_patched'] += 1
            stats['patched_dwords'] += len(found[3])
        shaders.append(item)
    out = dict(version=RECIPE_VERSION, title=TITLE, created=time.strftime('%Y-%m-%d %H:%M'),
               source_xsh=str(xsh), banks={n: dict(entries=len(e), sha256=hashlib.sha256(
                   (data_root(root) / 'data/Shaders' / n).read_bytes()).hexdigest()) for n, e in banks.items()},
               stats=dict(stats), unresolved=unresolved, shaders=shaders)
    if xpso:
        out['pipelines'] = dict(file='%s.metal.xpso' % TITLE, bytes=Path(xpso).stat().st_size,
                                data_b64=base64.b64encode(Path(xpso).read_bytes()).decode())
    return out


def rebuild(recipe_json, cache, root=None):
    """Write CACHE/shaders/shareable/4D5307F1.xsh (and .metal.xpso) from the recipe and this disc's banks."""
    r = json.loads(Path(recipe_json).read_text())
    if r.get('version') != RECIPE_VERSION: raise ValueError('recipe version %r, expected %d' % (r.get('version'), RECIPE_VERSION))
    banks = load_banks(data_root(root))
    out = bytearray(struct.pack('<II', XSH_MAGIC, int.from_bytes(struct.pack('>I', XSH_VERSION), 'little')))
    ok, bad = 0, []
    for s in r['shaders']:
        entries = banks.get(s['bank']) or next(iter(banks.values()))
        try:
            ucode = bytearray(entries[s['entry']][s['offset']:s['offset'] + 4 * s['dwords']])
        except IndexError:
            ucode = bytearray()
        for k, v in s['patches']:
            if 4 * k + 4 <= len(ucode): struct.pack_into('>I', ucode, 4 * k, v)
        if len(ucode) != 4 * s['dwords'] or hashlib.sha256(ucode).hexdigest() != s['sha256']:
            bad.append(s['hash'])
            continue
        out += struct.pack('<QI', int(s['hash'], 16), s['dwords'] | ((1 if s['type'] == 'pixel' else 0) << 31)) + ucode
        ok += 1
    shareable = Path(cache) / 'shaders/shareable'
    shareable.mkdir(parents=True, exist_ok=True)
    (shareable / ('%s.xsh' % TITLE)).write_bytes(bytes(out))
    if r.get('pipelines'):
        (shareable / r['pipelines']['file']).write_bytes(base64.b64decode(r['pipelines']['data_b64']))
    return dict(shaders=ok, mismatched=bad, unresolved=len(r.get('unresolved', [])),
                pipelines_bytes=(r.get('pipelines') or {}).get('bytes', 0), cache=str(cache))


def scan(root=None, xsh=None):
    root = data_root(root)
    started = time.monotonic()
    banks = load_banks(root)
    out = dict(data=str(root), banks={n: dict(entries=len(e), bytes=sum(map(len, e))) for n, e in banks.items()},
               containers=count_containers({BANKS[0]: banks.get(BANKS[0], [])}))
    if xsh:
        r = recipe(xsh, None, root, log=lambda *_: None)
        out['coverage'] = r['stats']
        out['coverage']['shaders'] = len(read_xsh(xsh))
    out['seconds'] = round(time.monotonic() - started, 2)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('--data')
    s.add_argument('--xsh')
    r = sub.add_parser('recipe')
    r.add_argument('--xsh', required=True)
    r.add_argument('--xpso')
    r.add_argument('--out', required=True)
    r.add_argument('--data')
    b = sub.add_parser('rebuild')
    b.add_argument('recipe')
    b.add_argument('--cache', required=True)
    b.add_argument('--data')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'scan':
            result = scan(a.data, a.xsh)
        elif a.cmd == 'recipe':
            rec = recipe(a.xsh, a.xpso, a.data)
            Path(a.out).parent.mkdir(parents=True, exist_ok=True)
            Path(a.out).write_text(json.dumps(rec) + '\n')
            result = dict(out=a.out, stats=rec['stats'], unresolved=rec['unresolved'], bytes=Path(a.out).stat().st_size)
        else:
            result = rebuild(a.recipe, a.cache, a.data)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
