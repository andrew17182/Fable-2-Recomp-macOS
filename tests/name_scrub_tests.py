#!/usr/bin/env python3
"""No personal name, personal email or home-folder path in any tracked file (release decision D5, 2026-10-14).

tools/fable2_name_guard.py holds the patterns; false positives go in tests/name_scrub_allowlist.txt. The portable /
benchmark-kit package check (tools/fable2_portable.py check_package) runs the same guard over every packaged file.
"""
import importlib.util
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fable2_name_guard', ROOT / 'tools/fable2_name_guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)

# Spelled in pieces so this file never matches itself.
FIRST, LAST, MAIL = 'An' + 'drew', 'Bu' + 'nch', 'gm' + 'ail.com'


def in_git_checkout():
    return shutil.which('git') is not None and subprocess.run(
        ['git', '-C', str(ROOT), 'rev-parse', '--is-inside-work-tree'], capture_output=True).returncode == 0


class GuardTests(unittest.TestCase):
    def labels(self, text, name='', allow=None):
        allow = guard.load_allowlist() if allow is None else allow
        return [label for label, _ in guard.find(text.encode(), name, allow)]

    def test_catches_each_identifier(self):
        self.assertEqual(self.labels('approved by %s on 2026-10-14' % FIRST), ['first name'])
        self.assertEqual(self.labels('%s\'s M3 Pro' % FIRST.upper()), ['first name'])
        self.assertIn('surname', self.labels('Copyright %s' % LAST))
        self.assertIn('personal email', self.labels('mail me at someone.else@%s' % MAIL))
        self.assertEqual(self.labels('/Us' + 'ers/somebody/projects/x.cpp'), ['home path'])

    def test_allows_placeholders_and_plain_words(self):
        self.assertEqual(self.labels('a %s of shaders' % LAST.lower()), [])
        self.assertEqual(self.labels('/Users/<name>/<workspace>, /Users/$USER, /Users/Shared/x'), [])

    def test_allowlist_keeps_the_github_handle(self):
        handle = FIRST.lower() + '17182'
        self.assertIn((None, handle.encode()), guard.load_allowlist())
        self.assertEqual(self.labels('https://github.com/%s/Fable-2-Recomp-macOS' % handle), [])
        self.assertEqual(self.labels('github.com/%s/x' % handle, allow=[]), ['first name'])

    def test_file_scoped_allowlist_entry(self):
        allow = [('docs/a.md', FIRST.encode())]
        self.assertEqual(self.labels('by %s' % FIRST, 'docs/a.md', allow), [])
        self.assertEqual(self.labels('by %s' % FIRST, 'docs/b.md', allow), ['first name'])


@unittest.skipUnless(in_git_checkout(), 'needs a git checkout')
class TrackedTreeTests(unittest.TestCase):
    def test_no_personal_identifiers_in_tracked_files(self):
        problems = guard.scan_tracked(ROOT)
        self.assertEqual(problems, [], 'personal identifiers in tracked files (rewrite them, or add a narrow entry '
                         'to tests/name_scrub_allowlist.txt):\n' + '\n'.join('%s:%d: %s %r' % p for p in problems))


if __name__ == '__main__':
    unittest.main()
