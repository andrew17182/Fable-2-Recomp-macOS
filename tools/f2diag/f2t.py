"""Reader for the flight recorder's binary files (ring.f2t dumps, stream.f2t).

Format (src/diagnostics/fable2_flight_recorder.cpp):
  dump:   b"F2T1" u32 header_len, header JSON, then blocks b"RING" u32 ring u64 first u32 count, count x 32 B
  stream: b"F2TS" u32 len, JSON (timebase, session_start), then chunks
          b"F2TC" u32 len, JSON (types, strings) followed by RING blocks (new records since the last chunk)
Records are 32 bytes, little-endian:
  event: u16 type, u8 thread, u8 flags, u32 frame, u64 t (mach ticks), u32 a, u32 b, u64 v
  cont:  u16 type (1 STACK / 2 TEXT), u8 thread, u8 flags, u8 n, u8 seq, u16 aux, 6 x u32 payload
Ring 0 is the multi-producer log ring: an event is valid only when v == its ring position, and its
TEXT continuations carry aux == position & 0xffff.
"""
import json
import struct
from pathlib import Path

CATEGORIES = ['frame', 'gpu', 'cpu', 'sampler', 'wait', 'signal', 'io', 'worker0', 'load', 'ui', 'shader',
              'draw', 'clock', 'input', 'log', 'mark', 'tag', 'crash', 'tex', 'shmem', 'save']
T_STACK, T_TEXT = 1, 2
F_BEGIN, F_END, F_HAS_STACK, F_TORN, F_HAS_TEXT, F_SELFTEST = 1, 2, 4, 8, 16, 32
STATES = {0: 'WORK', 1: 'SPIN', 2: 'WAIT'}
LOG_LEVELS = {0: 'trace', 1: 'debug', 2: 'info', 3: 'warning', 4: 'error', 5: 'critical'}
_EV = struct.Struct('<HBBIQIIQ')
_CONT = struct.Struct('<HBBBBH6I')


class Event:
    __slots__ = ('type', 'name', 'cat', 'thread', 'flags', 'frame', 't', 'a', 'b', 'v', 'stack', 'text', 'pos')

    def __init__(self, typ, thread, flags, frame, t, a, b, v, pos):
        self.type, self.thread, self.flags, self.frame, self.t = typ, thread, flags, frame, t
        self.a, self.b, self.v, self.pos = a, b, v, pos
        self.name = self.cat = ''
        self.stack = None
        self.text = None

    def __repr__(self):
        return f'<{self.name} t={self.t} thr={self.thread} a={self.a:#x} b={self.b} v={self.v}>'


class Trace:
    """One dump (or stream): header, events with stacks/text attached, clock conversion."""

    def __init__(self, header, blocks, path=None):
        self.path = Path(path) if path else None
        self.header = header
        tb = header.get('timebase', {'numer': 125, 'denom': 3})
        self.ns_per_tick = tb['numer'] / tb['denom']
        self.start = header.get('session_start') or header.get('clock') or {'mach': 0}
        self.types = {t['id']: t for t in header.get('types', [])}
        self.strings = header.get('strings', [])
        self.threads = {t['index']: t for t in header.get('threads', [])}
        self.guest_threads = {g['idx']: g for g in header.get('guest_threads', [])}
        self.events = []
        for ring, first, raw in blocks:
            self.events.extend(self._decode(ring, first, raw))
        self.events.sort(key=lambda e: e.t)

    # -- clocks
    def secs(self, t):
        """Seconds since the recorder started (session.json 'start')."""
        return (t - self.start['mach']) * self.ns_per_tick / 1e9

    def session_secs(self, t):
        """Seconds on the test-session clock (FABLE2_SESSION_ORIGIN_US, CLOCK_MONOTONIC), when known."""
        origin = self.header.get('session_origin_monotonic_us') or 0
        if not origin:
            return self.secs(t)
        mono_at_t = self.start['monotonic_us'] + (t - self.start['mach']) * self.ns_per_tick / 1000
        return (mono_at_t - origin) / 1e6

    def ms(self, ticks):
        return ticks * self.ns_per_tick / 1e6

    def thread_name(self, idx):
        t = self.threads.get(idx)
        return t['name'] if t and t.get('name') else f'ring{idx}'

    def guest_thread_name(self, idx):
        g = self.guest_threads.get(idx)
        return g['name'] if g else f'guest{idx}'

    # -- decoding
    def _decode(self, ring, first, raw):
        n = len(raw) // 32
        recs = [raw[i * 32:(i + 1) * 32] for i in range(n)]
        mpsc = self.threads.get(ring, {}).get('mpsc', False)
        out, i = [], 0
        while i < n:
            pos = first + i
            typ = struct.unpack_from('<H', recs[i])[0]
            if typ in (T_STACK, T_TEXT):  # orphan continuation (its event was overwritten)
                i += 1
                continue
            typ, thread, flags, frame, t, a, b, v = _EV.unpack(recs[i])
            if mpsc and v != pos:  # log ring: reserved but not (yet) written, or stale
                i += 1
                continue
            ev = Event(typ, thread, flags, frame, t, a, b, v, pos)
            info = self.types.get(typ)
            ev.name, ev.cat = (info['name'], info['cat']) if info else (f'type{typ}', '?')
            i += 1
            stack, text = [], bytearray()
            while i < n:
                ctyp, _, _, cn, seq, aux, *w = _CONT.unpack(recs[i])
                if ctyp not in (T_STACK, T_TEXT):
                    break
                if mpsc and aux != (pos & 0xFFFF):
                    break
                if ctyp == T_STACK:
                    stack.extend(w[:cn])
                else:
                    text.extend(recs[i][8:8 + cn])
                i += 1
            if flags & F_HAS_STACK:
                ev.stack = stack
            if flags & F_HAS_TEXT or text:
                ev.text = text.decode('utf-8', 'replace')
            out.append(ev)
        return out

    def by_name(self, *names):
        return [e for e in self.events if e.name in names]


def _read_blocks(data, off, stop_magic=None):
    blocks = []
    while off + 20 <= len(data) and data[off:off + 4] == b'RING':
        ring, first, count = struct.unpack_from('<IQI', data, off + 4)
        off += 20
        raw = data[off:off + count * 32]
        off += count * 32
        blocks.append((ring, first, raw))
    return blocks, off


def load(path):
    """Load a dump folder, a ring.f2t file or a stream.f2t file."""
    path = Path(path)
    if path.is_dir():
        path = path / 'ring.f2t'
    data = path.read_bytes()
    magic = data[:4]
    if magic == b'F2T1':
        (hlen,) = struct.unpack_from('<I', data, 4)
        header = json.loads(data[8:8 + hlen])
        blocks, _ = _read_blocks(data, 8 + hlen)
        return Trace(header, blocks, path)
    if magic == b'F2TS':
        (hlen,) = struct.unpack_from('<I', data, 4)
        header = json.loads(data[8:8 + hlen])
        off, blocks, latest = 8 + hlen, [], {}
        while off + 8 <= len(data) and data[off:off + 4] == b'F2TC':
            (clen,) = struct.unpack_from('<I', data, off + 4)
            latest.update(json.loads(data[off + 8:off + 8 + clen]))  # types/strings/threads only grow
            b, off = _read_blocks(data, off + 8 + clen)
            blocks.extend(b)
        header.update(latest)
        session = path.parent / 'session.json'
        if session.exists():
            s = json.loads(session.read_text())
            header.setdefault('session_origin_monotonic_us', s.get('session_origin_monotonic_us'))
        return Trace(header, blocks, path)
    raise ValueError(f'{path}: not a flight-recorder file (magic {magic!r})')
