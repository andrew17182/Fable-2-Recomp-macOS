#!/usr/bin/env python3
"""The AI-agent workflow docs stay out of the repo (release decision D5, 2026-10-14).

They live in a gitignored folder of the main checkout (tools/fable2_slots.py PRIVATE_DOCS, symlinked into every agent
slot). This test fails when that folder is not ignored, when a file in it is tracked, or when a tracked file links to
it or to one of the doc paths that moved there. Run: /usr/bin/python3 tests/private_docs_tests.py
"""
import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = 'docs-private'
# Paths the docs had before they left the repo (docs/ until 2026-10-13, docs/archive/ until 2026-10-14).
MOVED = ('agent-slots.md', 'playtest-build.md', 'heavy-tool-mode.md')
PATTERN = re.compile(r'%s/\S|(?:docs/|archive/)(?:%s)' % (re.escape(PRIVATE), '|'.join(re.escape(m) for m in MOVED)))
SELF = 'tests/private_docs_tests.py'


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT)] + list(args), capture_output=True)


def in_git_checkout():
    return shutil.which('git') is not None and git('rev-parse', '--is-inside-work-tree').returncode == 0


@unittest.skipUnless(in_git_checkout(), 'needs a git checkout')
class PrivateDocsTests(unittest.TestCase):
    def test_folder_and_link_are_ignored(self):
        # Both the real folder (main checkout) and a symlink to it (agent slots) must be ignored.
        result = git('check-ignore', '--no-index', '-q', PRIVATE)
        self.assertEqual(result.returncode, 0, '%s is not gitignored' % PRIVATE)
        rules = (ROOT / '.gitignore').read_text().splitlines()
        self.assertIn('/' + PRIVATE, rules, 'ignore pattern must have no trailing slash so the slot symlink matches')

    def test_nothing_private_is_tracked(self):
        tracked = git('ls-files', '-z').stdout.decode('utf-8', 'replace').split('\0')
        self.assertEqual([p for p in tracked if p == PRIVATE or p.startswith(PRIVATE + '/')], [])
        self.assertEqual([p for p in tracked if Path(p).name in MOVED], [])

    def test_no_tracked_file_links_private_docs(self):
        out = git('grep', '-n', '-I', '-E', PATTERN.pattern.replace('\\S', '[^[:space:]]').replace('(?:', '('),
                  '--', '.', ':!thirdparty').stdout.decode('utf-8', 'replace')
        hits = [line for line in out.splitlines() if line and line.split(':', 1)[0] != SELF]
        self.assertEqual(hits, [], 'tracked files link the private agent docs (say "local agent docs" instead):\n'
                         + '\n'.join(hits))

    def test_pattern(self):
        for text in ('see docs-private/agent-slots.md', '(docs/archive/heavy-tool-mode.md)', 'docs/playtest-build.md',
                     '[x](archive/agent-slots.md)'):
            self.assertTrue(PATTERN.search(text), text)
        for text in ('/docs-private', 'local agent docs', 'docs/archive/load-profile-20261006.md'):
            self.assertFalse(PATTERN.search(text), text)


if __name__ == '__main__':
    unittest.main()
