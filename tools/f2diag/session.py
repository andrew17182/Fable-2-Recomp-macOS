"""Locate a session's files: trace/ folder, dumps, main log, frame CSVs, test-control captures."""
import csv
import json
from pathlib import Path

from . import f2t


def resolve(path):
    """A path as given, or a bare session id (e.g. 20261013-203934-3d1c4ba8) looked up in out/dev-sessions/ of this
    checkout, the main checkout and every agent slot."""
    p = Path(path).expanduser()
    if p.exists() or len(p.parts) != 1:
        return p
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import fable2_common as common
    roots = [common.ROOT, common.MAIN_ROOT]
    try:
        roots += [Path(root) for _, root in common.checkouts()]
    except Exception:
        pass
    for root in roots:
        cand = Path(root) / 'out/dev-sessions' / p.name
        if cand.is_dir():
            return cand
    return p


def trace_dir(path):
    """Trace folder for a dev/test session folder, a state root, a logs folder, a trace folder or a dump."""
    p = resolve(path).resolve()
    for cand in (p, p / 'trace', p / 'logs/trace', p / 'test-state/logs/trace'):
        if (cand / 'session.json').exists() and (cand / 'dumps').is_dir():
            return cand
    if (p / 'ring.f2t').exists():  # a dump folder
        return p.parent.parent
    raise FileNotFoundError(f'no flight-recorder trace/ folder under {p}')


class Session:
    def __init__(self, path):
        self.trace = trace_dir(path)
        self.logs = self.trace.parent
        self.state = self.logs.parent
        self.meta = json.loads((self.trace / 'session.json').read_text())

    def dumps(self):
        return sorted(p for p in (self.trace / 'dumps').iterdir() if (p / 'ring.f2t').exists())

    def dump(self, which=None):
        """A dump folder: None = newest, an int = sequence number, a str = name substring (reason)."""
        ds = self.dumps()
        if not ds:
            raise FileNotFoundError(f'no dumps in {self.trace}')
        if which is None:
            return ds[-1]
        if isinstance(which, int) or str(which).isdigit():
            return next(d for d in ds if int(d.name.split('-')[0]) == int(which))
        return [d for d in ds if str(which) in d.name][-1]

    def load(self, which=None):
        """Trace of the newest (or chosen) dump, or the stream when which == 'stream'."""
        if which == 'stream':
            return f2t.load(self.trace / 'stream.f2t')
        return f2t.load(self.dump(which))

    def digest(self):
        p = self.trace / 'log-digest.jsonl'
        return [json.loads(x) for x in p.read_text().splitlines() if x.strip()] if p.exists() else []

    def log_path(self):
        return self.logs / 'fable_2.log'

    def frames_csv(self):
        files = sorted(self.logs.glob('frames-*.csv'))
        return files[-1] if files else None

    def frames(self):
        """Frame CSV rows as dicts (event, frame_id, time_us, duration_us, warmup)."""
        p = self.frames_csv()
        return list(csv.DictReader(open(p))) if p else []

    def captures(self):
        """Test-control capture sidecars (control/*.capture.json), sorted by capture time."""
        out = []
        for p in sorted((self.state / 'control').glob('*.capture.json')):
            try:
                m = json.loads(p.read_text())
            except ValueError:
                continue
            m['path'] = str(p.with_name(p.name.replace('.capture.json', '.ppm')))
            out.append(m)
        out.sort(key=lambda m: m.get('capture_started_monotonic_us') or 0)
        return out

    def writeback(self):
        p = self.state.parent / 'writeback.jsonl'
        return [json.loads(x) for x in p.read_text().splitlines() if x.strip()] if p.exists() else []


def host_session(path):
    """The dev/test session folder (holds session.json with the launch record) for any path inside it."""
    p = resolve(path).resolve()
    for cand in [p] + list(p.parents):
        if (cand / 'session.json').is_file() and (cand / 'test-state').is_dir():
            return cand
    return None


def contention(path):
    """Other game instances that ran beside this session: dict(count, others[str], text). Sources: the launch
    record (concurrent_instances), the watcher's instances.jsonl, else the flight-recorder 'contention' tag."""
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import fable2_instances as instances
    seen, session = {}, host_session(path)
    if session:
        try:
            for e in json.loads((session / 'session.json').read_text()).get('concurrent_instances') or []:
                seen[e['pid']] = e
        except (OSError, ValueError):
            pass
        try:
            for line in (session / 'instances.jsonl').read_text().splitlines():
                for e in json.loads(line).get('others', []): seen[e['pid']] = e
        except (OSError, ValueError):
            pass
    others = sorted(seen.values(), key=lambda e: e['pid'])
    if others:
        return dict(count=len(others), others=[instances.describe(e) for e in others],
                    text=instances.contention_text(others))
    tag = None
    try:
        tag = Session(path).meta.get('tags', {}).get('contention')
    except (OSError, ValueError, FileNotFoundError):
        pass
    if tag not in (None, '', '0', 0, -1):
        return dict(count=None, others=[str(tag)], text=f'CONTENTION: {tag} (flight-recorder tag) — perf numbers not comparable')
    return dict(count=0, others=[], text='')
