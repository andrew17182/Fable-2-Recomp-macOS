#!/usr/bin/env python3
"""The AI-agent workflow docs stay out of the repo (release decision D5, 2026-10-14).

They live in a gitignored folder of the main checkout (tools/fable2_slots.py PRIVATE_DOCS, symlinked into every agent
slot). This test fails when that folder is not ignored, when a file in it is tracked, or when a tracked file links to
it or to one of the doc paths that moved there. Run: /usr/bin/python3 tests/private_docs_tests.py

The same holds for the other local-only files (2026-10-16, one repo): the paused save-state work in the gitignored
`private/` folder and the internal audits in the private docs folder. Their names are read from disk (never listed in a
tracked file), so the name check runs in the main checkout and is skipped where those folders are absent. Every
relative Markdown link in a tracked doc must resolve.
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
# Local-only save-state work (paths relative to the repo root are kept below it) and internal audits.
PRIVATE_CODE = 'private'
INTERNAL_DOCS = PRIVATE + '/internal'
LINK = re.compile(r'\]\(([\w./-]+?)(?:#[\w-]*)?\)')


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

    def test_private_code_folder_is_ignored_and_untracked(self):
        self.assertEqual(git('check-ignore', '--no-index', '-q', PRIVATE_CODE).returncode, 0,
                         '%s is not gitignored' % PRIVATE_CODE)
        tracked = git('ls-files', '-z').stdout.decode('utf-8', 'replace').split('\0')
        self.assertEqual([p for p in tracked if p.startswith(PRIVATE_CODE + '/')], [])

    def test_no_tracked_file_names_local_only_files(self):
        # Docs (*.md) may not name any local-only file or folder; other files may not name a local-only doc (opt-in,
        # #ifdef-guarded save-state includes in code are fine: the build proves they are not compiled).
        tracked = git('ls-files').stdout.decode('utf-8', 'replace').splitlines()
        tracked_dirs = {str(d) for p in tracked for d in Path(p).parents}
        docs, code, folders = set(), set(), set()
        for base in (ROOT / PRIVATE_CODE, ROOT / INTERNAL_DOCS):
            for f in (base.rglob('*') if base.is_dir() else ()):
                if not f.is_file() or f.name.startswith('.') or f.name.startswith('README'):
                    continue
                (docs if f.suffix == '.md' else code).add(f.name)
                if base.name == PRIVATE_CODE:
                    parts = f.relative_to(base).parts[1:-1]  # below <work>/: the path it had in the repo
                    for i in range(1, len(parts) + 1):
                        d = '/'.join(parts[:i])
                        if d not in tracked_dirs:
                            folders.add(d + '/')
                            break
        if not (docs or code):
            self.skipTest('no local-only files on disk (fresh clone)')
        hits = []
        for f in tracked:
            p = ROOT / f
            if f.startswith('thirdparty/') or f == SELF or p.is_symlink() or not p.is_file():
                continue
            text = p.read_text(errors='ignore')
            for n in sorted(docs | code | folders) if f.endswith('.md') else sorted(docs):
                if re.search(r'(?<![\w-])' + re.escape(n), text):
                    hits.append('%s -> %s' % (f, n))
        self.assertEqual(hits, [], 'tracked files name local-only files:\n' + '\n'.join(hits))

    def test_markdown_links_resolve(self):
        broken = []
        for f in git('ls-files', '*.md').stdout.decode('utf-8', 'replace').splitlines():
            p = ROOT / f
            if f.startswith('thirdparty/') or not p.is_file():
                continue
            for target in LINK.findall(p.read_text(errors='ignore')):
                if re.match(r'^[a-zA-Z]+:', target) or target.startswith('#'):
                    continue
                if not (p.parent / target).exists():
                    broken.append('%s -> %s' % (f, target))
        self.assertEqual(broken, [], 'broken relative links:\n' + '\n'.join(broken))

    def test_pattern(self):
        for text in ('see docs-private/agent-slots.md', '(docs/archive/heavy-tool-mode.md)', 'docs/playtest-build.md',
                     '[x](archive/agent-slots.md)'):
            self.assertTrue(PATTERN.search(text), text)
        for text in ('/docs-private', 'local agent docs', 'docs/archive/load-profile-20261006.md'):
            self.assertFalse(PATTERN.search(text), text)


if __name__ == '__main__':
    unittest.main()
