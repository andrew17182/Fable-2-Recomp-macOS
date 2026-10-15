#!/usr/bin/env python3
"""Personal-identifier guard: no maintainer name, personal email or home-folder path in the repo or in a package.

Release decision D5 (2026-10-14): no personal name anywhere in the repo. Used by
tests/name_scrub_tests.py (every tracked file) and by fable2_portable.check_package (every file of the portable
app and the benchmark kit). False positives go in tests/name_scrub_allowlist.txt.

    /usr/bin/python3 tools/fable2_name_guard.py            # scan tracked files, exit 1 on a hit
    /usr/bin/python3 tools/fable2_name_guard.py PATH...     # scan files or folders (e.g. an unpacked zip)

Runs under /usr/bin/python3 (3.9).
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / 'tests/name_scrub_allowlist.txt'

# Spelled in pieces so this file never matches itself.
_FIRST = 'an' + 'drew'
_LAST = 'Bu' + 'nch'
_MAIL = 'gm' + 'ail'
PATTERNS = [
    ('first name', re.compile(_FIRST.encode(), re.I)),
    # Case-sensitive whole word: the English "a bunch of" stays allowed.
    ('surname', re.compile(rb'\b' + _LAST.encode() + rb'\b')),
    ('personal email', re.compile(rb'[\w.+-]+@' + _MAIL.encode() + rb'\.com\b', re.I)),
    # A real home folder; placeholders such as /Users/<name> or /Users/$USER, and /Users/Shared, are fine.
    ('home path', re.compile(rb'/Users/(?!Shared\b)[A-Za-z0-9_][\w.-]*')),
]


def load_allowlist(path=ALLOWLIST):
    """Allowlist entries as (file or None, literal bytes). A line is either `literal` (ignored in every file) or
    `path/in/repo | literal` (ignored in that file only). Blank lines and `#` comments are skipped."""
    entries = []
    try:
        lines = Path(path).read_text().splitlines()
    except OSError:
        return entries
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        where, sep, literal = line.partition(' | ')
        entries.append((where.strip(), literal.strip().encode()) if sep else (None, line.encode()))
    return entries


def find(data, name='', allow=()):
    """[(label, matched text)] for every identifier in `data` (bytes) after removing allowlisted literals."""
    for where, literal in allow:
        if literal and (where is None or where == name):
            data = re.sub(re.escape(literal), b' ' * len(literal), data, flags=re.I)
    hits = []
    for label, pattern in PATTERNS:
        hits.extend((label, m.group(0).decode('utf-8', 'replace')) for m in pattern.finditer(data))
    return hits


def tracked_files(root=ROOT):
    out = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'], capture_output=True, check=True).stdout
    return [p for p in out.decode().split('\0') if p]


def scan_tracked(root=ROOT, allow=None):
    """[(path, line number, label, matched text)] over every tracked file (file names included)."""
    allow = load_allowlist() if allow is None else allow
    problems = []
    for rel in tracked_files(root):
        for label, text in find(rel.encode(), rel, allow):
            problems.append((rel, 0, label, text))
        path = Path(root) / rel
        if not path.is_file():  # submodule gitlink
            continue
        data = path.read_bytes()
        for number, line in enumerate(data.split(b'\n'), 1):
            for label, text in find(line, rel, allow):
                problems.append((rel, number, label, text))
    return problems


def scan_paths(paths, allow=None):
    """[(file, label, matched text)] over files and folders (recursively)."""
    allow = load_allowlist() if allow is None else allow
    problems = []
    for top in map(Path, paths):
        for path in [top] if top.is_file() else sorted(p for p in top.rglob('*') if p.is_file()):
            for label, text in find(path.read_bytes(), '', allow):
                problems.append((str(path), label, text))
    return problems


def main(argv):
    if argv:
        problems = ['%s: %s %r' % p for p in scan_paths(argv)]
    else:
        problems = ['%s:%d: %s %r' % p for p in scan_tracked()]
    for line in problems:
        print(line)
    print('%d personal identifier(s) found' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
