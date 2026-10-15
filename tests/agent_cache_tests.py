"""Agent shader cache (tools/fable2_agent_cache.py): key and seed selection, the publish guard,
frozen snapshots, cold seeds and pruning. Everything runs in scratch folders (never the live state or out/agent-cache).
Run: /usr/bin/python3 tests/agent_cache_tests.py"""
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import fable2_agent_cache as ac  # noqa: E402
import fable2_common as common  # noqa: E402

CLEAN = ac.CLEAN_EXIT_MARKER + '\n'
SEG = 'shaders/local/metal/GPU/abi/4D5307F1/4D5307F1.rtv-bindless.os27.metal.seg%06d.binarchive'
STORE = 'shaders/local/metal/GPU/abi/4D5307F1/4D5307F1.metal.artifacts'


def write(path, data, mtime=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    if mtime: os.utime(path, (mtime, mtime))


def tree(root):
    root = Path(root)
    return {str(p.relative_to(root)): p.read_bytes() for p in sorted(root.rglob('*')) if p.is_file()}


class AgentCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='agent-cache-tests-'))
        self.saved = (ac.CACHE_ROOT, ac.LIVE_STATE, common.LOCKS, ac.branch_fingerprints)
        ac.CACHE_ROOT = self.tmp / 'agent-cache'
        ac.LIVE_STATE = self.tmp / 'live'
        common.LOCKS = self.tmp / 'locks'
        # A "live" cache: a store, two archive segments, a learn file (mtimes in the past, like a real cache).
        old = time.time() - 3600
        write(ac.LIVE_STATE / 'cache' / STORE, b'A' * 1000, old)
        write(ac.LIVE_STATE / 'cache' / (SEG % 1), b'seg1', old)
        write(ac.LIVE_STATE / 'cache' / (SEG % 2), b'seg2', old)
        write(ac.LIVE_STATE / 'cache' / 'shaders/shareable/4D5307F1.xsh', b'xsh', old)
        self.live_before = self.live_stat()
        self.fp = dict(key='aaaaaaaaaaaaaaaa', components=dict(translator='t1', optimizer='o1', ns_spec='n1',
                                                               pipeline='p1', toolchain='c1'))
        self.branch_fp = self.fp
        ac.branch_fingerprints = lambda root=None: {'perf-staging': self.branch_fp}
        self.n = 0

    def tearDown(self):
        # The live cache is only ever read.
        self.assertEqual(self.live_stat(), self.live_before)
        ac.CACHE_ROOT, ac.LIVE_STATE, common.LOCKS, ac.branch_fingerprints = self.saved
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def live_stat(self):
        root = ac.LIVE_STATE / 'cache'
        return {str(p.relative_to(root)): (p.stat().st_size, p.stat().st_mtime_ns, p.read_bytes())
                for p in sorted(root.rglob('*')) if p.is_file()}

    def app(self, fp):
        self.n += 1
        folder = self.tmp / ('build%d' % self.n)
        (folder / 'Fable II.app').mkdir(parents=True)
        (folder / 'metal-build.json').write_text(json.dumps(dict(shader_cache_key=dict(fp, inputs=[]))))
        return folder / 'Fable II.app'

    def session(self, app, mode=None):
        self.n += 1
        state = self.tmp / ('s%d' % self.n) / 'test-state'
        state.mkdir(parents=True)
        rec = ac.seed(state / 'cache', app, mode)
        return state, rec

    def finish(self, state, clean=True):
        write(state / 'logs/fable_2.log', CLEAN.encode() if clean else b'crash\n')

    # ------------------------------------------------------------------ fingerprint
    def test_fingerprint_components(self):
        files = {
            ac.METAL + 'metal_air_optimizer.cc': b'opt',
            'backends/metal/sdk-metal.patch': (b'--- a/src/graphics/pipeline/shader/dxbc_translator.cpp\n+++ b/x\n@@\n+a\n'
                                               b'--- a/src/graphics/pipeline/render_target/cache.cpp\n+++ b/y\n@@\n+b\n'),
        }
        base = ac.fingerprint(files.get)
        self.assertRegex(base['key'], r'^[0-9a-f]{16}$')
        changed = dict(files)
        changed[ac.METAL + 'metal_air_optimizer.cc'] = b'opt2'
        fp2 = ac.fingerprint(changed.get)
        self.assertNotEqual(fp2['key'], base['key'])
        self.assertEqual([n for n in base['components'] if base['components'][n] != fp2['components'][n]], ['optimizer'])
        # A non-shader hunk of an SDK patch (render targets) does not change the key; a shader hunk does.
        rt = dict(files)
        rt['backends/metal/sdk-metal.patch'] = files['backends/metal/sdk-metal.patch'].replace(b'+b\n', b'+c\n')
        self.assertEqual(ac.fingerprint(rt.get)['key'], base['key'])
        sh = dict(files)
        sh['backends/metal/sdk-metal.patch'] = files['backends/metal/sdk-metal.patch'].replace(b'+a\n', b'+z\n')
        self.assertNotEqual(ac.fingerprint(sh.get)['key'], base['key'])

    # ------------------------------------------------------------------ seed selection
    def test_auto_seed_creates_key_from_live_and_publishes(self):
        app = self.app(self.fp)
        state, rec = self.session(app)
        self.assertEqual((rec['key'], rec['generation'], rec['publish']), (self.fp['key'], 0, True))
        self.assertEqual(tree(state / 'cache'), tree(ac.LIVE_STATE / 'cache'))
        self.assertTrue(rec['signature'].startswith(self.fp['key'] + '@0:'))

    def test_experimental_build_reads_closest_and_never_writes(self):
        self.session(self.app(self.fp))  # creates the baseline key
        exp = dict(key='bbbbbbbbbbbbbbbb', components=dict(self.fp['components'], optimizer='o2'))
        state, rec = self.session(self.app(exp))
        self.assertEqual(rec['key'], self.fp['key'])
        self.assertFalse(rec['publish'])
        self.assertFalse(ac.key_dir(exp['key']).exists())
        write(state / 'cache/new.bin', b'x')
        self.finish(state)
        self.assertEqual(ac.publish(state, rec)['result'], 'skipped')
        self.assertEqual(ac.load_meta(self.fp['key'])['generation'], 0)

    def test_no_key_and_no_toolchain_match_falls_back_to_live_clone(self):
        exp = dict(key='cccccccccccccccc', components=dict(self.fp['components'], toolchain='c2'))
        state, rec = self.session(self.app(exp))
        self.assertIsNone(rec['key'])
        self.assertEqual(rec['source'], 'live clone')
        self.assertEqual(tree(state / 'cache'), tree(ac.LIVE_STATE / 'cache'))
        self.assertFalse(ac.CACHE_ROOT.exists() and ac.keys())

    def test_build_without_key_is_live_clone_read_only(self):
        app = self.tmp / 'old/Fable II.app'
        app.mkdir(parents=True)
        state, rec = self.session(app)
        self.assertEqual((rec['source'], rec['publish']), ('live clone', False))

    def test_cold_seed_stays_empty(self):
        app = self.app(self.fp)
        state, rec = self.session(app, 'cold')
        self.assertEqual(list((state / 'cache').iterdir()), [])
        self.assertFalse(rec['publish'])
        write(state / 'cache' / STORE, b'B' * 10)
        self.finish(state)
        self.assertEqual(ac.publish(state, rec)['result'], 'skipped')
        self.assertFalse(ac.keys())  # cold never created or grew a key

    def test_off_is_live_clone(self):
        state, rec = self.session(self.app(self.fp), 'off')
        self.assertEqual(tree(state / 'cache'), tree(ac.LIVE_STATE / 'cache'))
        self.assertFalse(rec['publish'])

    # ------------------------------------------------------------------ publish guard
    def test_publish_appends_and_guards(self):
        app = self.app(self.fp)
        state, rec = self.session(app)
        write(state / 'cache' / STORE, b'A' * 1000 + b'more')  # appended
        write(state / 'cache' / (SEG % 3), b'seg3')              # new segment
        (state / 'cache' / (SEG % 1)).unlink()                  # compaction dropped a segment
        write(state / 'cache' / 'tmpfile.tmp-123', b'partial')  # never published
        # Crashed session: nothing.
        self.finish(state, clean=False)
        self.assertEqual(ac.publish(state, rec)['result'], 'skipped')
        self.finish(state)
        res = ac.publish(state, rec, 'sess')
        self.assertEqual(res['result'], 'published')
        self.assertEqual(res['generation'], 1)
        self.assertEqual(sorted(res['updated']), sorted([STORE, SEG % 3]))
        self.assertEqual(res['deleted'], [SEG % 1])
        g1 = tree(ac.gen_dir(self.fp['key'], 1))
        self.assertEqual(g1[STORE], b'A' * 1000 + b'more')
        self.assertNotIn(SEG % 1, g1)
        self.assertNotIn('tmpfile.tmp-123', g1)
        # Generation 0 is immutable.
        self.assertEqual(tree(ac.gen_dir(self.fp['key'], 0))[STORE], b'A' * 1000)
        # The next auto session seeds generation 1.
        state2, rec2 = self.session(app)
        self.assertEqual(rec2['generation'], 1)
        self.assertEqual(tree(state2 / 'cache'), g1)

    def test_rewritten_file_is_not_published(self):
        state, rec = self.session(self.app(self.fp))
        write(state / 'cache' / STORE, b'Z' * 2000)  # not an extension
        self.finish(state)
        res = ac.publish(state, rec)
        self.assertEqual(res['result'], 'unchanged')
        self.assertIn(STORE, res['skipped'])

    def test_readonly_mode_never_publishes(self):
        state, rec = self.session(self.app(self.fp), 'readonly')
        self.assertEqual(rec['key'], self.fp['key'])
        write(state / 'cache/new.bin', b'x')
        self.finish(state)
        self.assertEqual(ac.publish(state, rec)['result'], 'skipped')

    def test_concurrent_sessions_merge(self):
        app = self.app(self.fp)
        s1, r1 = self.session(app)
        s2, r2 = self.session(app)
        for s, tag in ((s1, b'one'), (s2, b'two')):
            write(s / 'cache' / STORE, b'A' * 1000 + tag)
            write(s / 'cache' / ('only-%s.bin' % tag.decode()), tag)
            self.finish(s)
        self.assertEqual(ac.publish(s1, r1)['generation'], 1)
        res2 = ac.publish(s2, r2)
        self.assertEqual(res2['generation'], 2)
        self.assertEqual(res2['updated'], ['only-two.bin'])
        self.assertIn(STORE, res2['skipped'])  # the other session's growth wins; no corruption
        g2 = tree(ac.gen_dir(self.fp['key'], 2))
        self.assertEqual(g2[STORE], b'A' * 1000 + b'one')
        self.assertIn('only-one.bin', g2)

    # ------------------------------------------------------------------ frozen snapshots
    def test_pinned_snapshot_is_identical_and_survives_prune(self):
        app = self.app(self.fp)
        s, r = self.session(app)
        snap = ac.pin('ab-batch', app=app)
        self.assertEqual(snap['snapshot'], self.fp['key'] + '@0')
        frozen = tree(ac.gen_dir(self.fp['key'], 0))
        # The cache grows (several publishes) while the batch runs.
        for i in range(4):
            st, rr = self.session(app)
            write(st / 'cache' / ('grow%d.bin' % i), b'g')
            self.finish(st)
            self.assertEqual(ac.publish(st, rr)['result'], 'published')
        self.assertEqual(ac.load_meta(self.fp['key'])['generation'], 4)
        self.assertTrue(ac.gen_dir(self.fp['key'], 0).is_dir())  # pinned: not pruned
        self.assertFalse(ac.gen_dir(self.fp['key'], 1).is_dir())  # old, unpinned: pruned
        for spec in ('ab-batch', snap['snapshot']):
            a_state, a_rec = self.session(app, spec)
            self.assertEqual(tree(a_state / 'cache'), frozen)
            self.assertEqual((a_rec['key'], a_rec['generation']), (self.fp['key'], 0))
        # Arms with an experimental build get the very same snapshot.
        exp = dict(key='dddddddddddddddd', components=dict(self.fp['components'], ns_spec='n9'))
        e_state, e_rec = self.session(self.app(exp), 'ab-batch')
        self.assertEqual(tree(e_state / 'cache'), frozen)
        self.assertFalse(e_rec['publish'])
        self.assertEqual(ac.unpin('ab-batch'), [self.fp['key'] + '/ab-batch'])
        ac.prune()
        self.assertFalse(ac.gen_dir(self.fp['key'], 0).is_dir())
        with self.assertRaises(ValueError):
            self.session(app, self.fp['key'] + '@0')

    # ------------------------------------------------------------------ housekeeping
    def test_prune_size_cap_and_age(self):
        self.session(self.app(self.fp))
        other = dict(key='eeeeeeeeeeeeeeee', components=dict(self.fp['components'], translator='t2'))
        self.branch_fp = other
        self.session(self.app(other))
        self.assertEqual(sorted(ac.keys()), sorted([self.fp['key'], other['key']]))
        meta = ac.load_meta(self.fp['key'])
        meta['last_used'] = time.time() - 30 * 86400
        ac._atomic_json(ac.key_dir(self.fp['key']) / 'meta.json', meta)
        removed = ac.prune(dry_run=True)
        self.assertTrue(any(r.startswith(self.fp['key'] + ' ') for r in removed))
        self.assertEqual(len(ac.keys()), 2)  # dry run
        ac.prune()
        self.assertEqual(ac.keys(), [other['key']])  # the most recently used key always stays
        os.environ['FABLE2_AGENT_CACHE_MAX_GB'] = '0'
        try:
            ac.prune()
            self.assertEqual(ac.keys(), [other['key']])
        finally:
            del os.environ['FABLE2_AGENT_CACHE_MAX_GB']
        self.assertIn('Agent shader cache: 1 key', ac.status_lines()[0])

    def test_wait_published(self):
        sess = self.tmp / 'sessw'
        sess.mkdir()
        (sess / 'session.json').write_text(json.dumps(dict(agent_cache=dict(publish=False))))
        self.assertIsNone(ac.wait_published(sess, timeout=0.2))
        (sess / 'session.json').write_text(json.dumps(dict(agent_cache=dict(publish=True))))
        (sess / 'writeback.jsonl').write_text(json.dumps(dict(event='game-exited')) + '\n' +
                                              json.dumps(dict(event=ac.EVENT, result='published', generation=3)) + '\n')
        self.assertEqual(ac.wait_published(sess, timeout=1)['generation'], 3)


if __name__ == '__main__':
    unittest.main()
