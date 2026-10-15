#!/usr/bin/env python3
"""List getenv() calls that are not read once into a static.

A getenv() walks the whole environment (__findenv_locked) on every call. On a per-draw path it cost 5-8% of the
GPU Commands thread (docs/bug-patterns.md, "getenv on a hot path"). Read env switches once:

    static const bool on = [] { const char* v = std::getenv("FABLE2_X"); return v && v[0] == '1'; }();

A site counts as "static" when the getenv sits inside a `static ... = [...] {` initializer (looked up to 40 lines back,
brace-matched) or on a `static` declaration line. Every other site is listed. Sites that are one-time by construction
(startup, setup, shutdown, a cold error path) can be marked with a `getenv-ok: <reason>` comment on the same line or
the line above; they are then listed only with --all.

Usage: python3 tools/getenv_audit.py [--all] [--strict] [paths...]   (default: backends/metal src)
--strict exits 1 when an unmarked non-static site exists (for a pre-merge check).
Runs under /usr/bin/python3 3.9.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTS = ('.cc', '.cpp', '.mm', '.h', '.hpp', '.c', '.m')
GETENV = re.compile(r'\b(?:std::)?getenv\s*\(')
STATIC_INIT = re.compile(r'\bstatic\b[^;]*=\s*\[[^\]]*\]\s*(?:\([^)]*\))?\s*(?:->\s*[\w:<>]+\s*)?\{')


def in_static_init(lines, i):
    if i > 0 and re.search(r'\bstatic\b', lines[i - 1]) and lines[i - 1].rstrip().endswith('='):
        return True
    if re.search(r'\bstatic\b', lines[i]) and '=' in lines[i].split('getenv')[0]:
        return True
    # Walk back: find a static lambda initializer whose body still encloses line i.
    for j in range(i, max(-1, i - 40), -1):
        m = STATIC_INIT.search(lines[j])
        if not m:
            continue
        depth = 0
        start = lines[j][m.end() - 1:]
        text = start + '\n' + '\n'.join(lines[j + 1:i])
        text += '\n' + lines[i].split('getenv')[0]
        for ch in text:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
        return depth > 0
    return False


FUNC = re.compile(r'(?:^|\s)([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{?\s*$')


def enclosing_function(lines, i):
    """Name of the small function around line i (looked up to 6 lines back), or None."""
    for j in range(i, max(-1, i - 7), -1):
        text = lines[j].split('{')[0] + ('{' if '{' in lines[j] else '')
        m = FUNC.search(text.rstrip())
        if m and m.group(1) not in ('if', 'while', 'for', 'switch', 'return', 'getenv'):
            return m.group(1)
    return None


def static_only_helper(lines, i):
    """True when the getenv sits in a helper whose every call in this file is a static initializer."""
    name = enclosing_function(lines, i)
    if not name:
        return False
    call = re.compile(r'\b' + re.escape(name) + r'\s*\(')
    calls = [k for k, line in enumerate(lines)
             if call.search(line) and not line.strip().startswith('//') and k != i]
    calls = [k for k in calls if not FUNC.search(lines[k].split('{')[0] + '{')
             or 'static const' in lines[k] or '=' in lines[k]]
    return bool(calls) and all(in_static_init(lines, k) for k in calls)


def marked(lines, i):
    return 'getenv-ok' in lines[i] or (i > 0 and 'getenv-ok' in lines[i - 1])


def main(argv):
    show_all = '--all' in argv
    strict = '--strict' in argv
    paths = [a for a in argv if not a.startswith('--')] or ['backends/metal', 'src']
    unmarked = 0
    for base in paths:
        base = os.path.join(ROOT, base)
        for dirpath, dirnames, files in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in ('build', 'out', 'third_party', 'thirdparty')]
            for f in sorted(files):
                if not f.endswith(EXTS):
                    continue
                p = os.path.join(dirpath, f)
                with open(p, errors='replace') as fh:
                    lines = fh.read().split('\n')
                for i, line in enumerate(lines):
                    s = line.strip()
                    if s.startswith('//') or s.startswith('*') or not GETENV.search(line):
                        continue
                    if in_static_init(lines, i) or static_only_helper(lines, i):
                        continue
                    ok = marked(lines, i)
                    if ok and not show_all:
                        continue
                    unmarked += 0 if ok else 1
                    rel = os.path.relpath(p, ROOT)
                    print(f"{rel}:{i + 1}: {'ok   ' if ok else 'CHECK'} {s[:120]}")
    print(f"{unmarked} unmarked non-static getenv site(s)", file=sys.stderr)
    return 1 if strict and unmarked else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
