"""Guest address symbolizer.

The start address of every recompiled function comes from generated/default/fable_2_register.cpp
(registrar->SetFunction(0x822D74A0, KeWaitForSingleObject_Guest)); most names also end in their address
(YieldAndCheckThreshold_82CBD098), which is the fallback from fable_2_funcs.*.h. The sorted table is
cached in ~/.cache/f2diag/symbols-<mtime>.json (generated/ is local-only and may be absent).
"""
import bisect
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_DECL = re.compile(r'DECLARE_REX_FUNC\((\w+?_([0-9A-F]{8}))\)')
_REG = re.compile(r'SetFunction\(0x([0-9A-Fa-f]{8}),\s*(\w+)\)')
_table = None


def _cache_path(src):
    stamp = max((p.stat().st_mtime_ns for p in src), default=0)
    base = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'f2diag'
    return base / f'symbols-{stamp}.json'


def table():
    """Sorted [(address, name)] (empty when generated/ is missing)."""
    global _table
    if _table is not None:
        return _table
    gen = ROOT / 'generated/default'
    src = [gen / 'fable_2_register.cpp'] if (gen / 'fable_2_register.cpp').exists() else sorted(gen.glob('fable_2_funcs.*.h'))
    cache = _cache_path(src)
    if cache.exists():
        _table = [tuple(x) for x in json.loads(cache.read_text())]
        return _table
    names = {}
    for p in src:
        text = p.read_text(errors='replace')
        for m in _REG.finditer(text):
            names[int(m[1], 16)] = m[2]
        for m in _DECL.finditer(text):
            names.setdefault(int(m[2], 16), m[1])
    _table = sorted(names.items())
    try:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps(_table))
    except OSError:
        pass
    return _table


def name(addr, offset=True):
    """'Func_82CBD098+0x10' for a return address (or the hex address when unknown)."""
    t = table()
    if not t or not addr:
        return f'{addr:08X}'
    i = bisect.bisect_right(t, (addr, '￿')) - 1
    if i < 0:
        return f'{addr:08X}'
    start, fn = t[i]
    if addr - start > 0x40000:  # past any plausible function body
        return f'{addr:08X}'
    return f'{fn}+0x{addr - start:X}' if offset and addr != start else fn


def chain(addrs, limit=None, offset=False):
    return ' < '.join(name(a, offset) for a in list(addrs)[:limit])
