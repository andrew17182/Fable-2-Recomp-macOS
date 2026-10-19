"""Release app pieces that run without a game build (docs/release-app.md):

- tools/fable2_game_build.py: the kit sync keeps unchanged files (and their mtimes, so make stays incremental),
  re-copies changed ones with a new mtime, removes files a newer kit dropped and never touches anything else in the
  build tree; the RAM-based job rule; status() against a kit;;
- tools/apply_recomp_patches.py: the per-file passes give the same files and counts in worker processes as in one;
- tools/fable2_release.py: check_release refuses game-derived files and build output in a package; the release zip
  has no __MACOSX/._ (AppleDouble) entries, even when the files carry extended attributes; no unsealed signing.
Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fable2_game_build as gb  # noqa: E402


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.kit, self.dest, self.record = self.root / 'kit', self.root / 'dest', self.root / 'sync.json'

    def tearDown(self):
        self.temp.cleanup()

    def kit_files(self, files):
        for rel, text in files.items(): write(self.kit / rel, text)
        return {rel: gb.sha256_file(self.kit / rel) for rel in files}

    def test_incremental_sync(self):
        files = self.kit_files({'a.h': 'a', 'src/b.cpp': 'b', 'old.txt': 'gone soon'})
        self.assertEqual(gb.sync_tree(self.kit, self.dest, files, self.record), (3, 0))
        write(self.dest / 'generated/x.cpp', 'generated')  # not a kit file: never touched
        old = time.time() - 3600
        os.utime(self.dest / 'a.h', (old, old))
        os.utime(self.dest / 'src/b.cpp', (old, old))
        (self.kit / 'old.txt').unlink()
        files = self.kit_files({'a.h': 'a', 'src/b.cpp': 'b changed'})
        self.assertEqual(gb.sync_tree(self.kit, self.dest, files, self.record), (1, 1))
        self.assertAlmostEqual((self.dest / 'a.h').stat().st_mtime, old, delta=1)  # unchanged: mtime kept
        self.assertGreater((self.dest / 'src/b.cpp').stat().st_mtime, old + 3000)  # changed: newer than objects
        self.assertEqual((self.dest / 'src/b.cpp').read_text(), 'b changed')
        self.assertFalse((self.dest / 'old.txt').exists())
        self.assertEqual((self.dest / 'generated/x.cpp').read_text(), 'generated')

    def test_damaged_copy_is_replaced(self):
        files = self.kit_files({'a.h': 'aaaa'})
        gb.sync_tree(self.kit, self.dest, files, self.record)
        (self.dest / 'a.h').write_text('a')  # size differs from the record
        self.assertEqual(gb.sync_tree(self.kit, self.dest, files, self.record), (1, 0))
        self.assertEqual((self.dest / 'a.h').read_text(), 'aaaa')


class PlanTests(unittest.TestCase):
    def jobs(self, ram, cores):
        return gb.plan_jobs(dict(ram_gb=ram, cores=cores, perf_cores=4, model='', chip=''))['jobs']

    def test_job_rule(self):
        self.assertEqual(self.jobs(8, 8), 6)     # 8 GB M1: (8 - 3.5) / 0.75
        self.assertEqual(self.jobs(16, 8), 8)    # limited by cores
        self.assertEqual(self.jobs(16, 12), 12)
        self.assertEqual(self.jobs(64, 18), 18)
        self.assertEqual(self.jobs(4, 8), 1)     # never below one
        self.assertEqual(gb.plan_jobs(override='3')['jobs'], 3)


class StatusTests(unittest.TestCase):
    def test_ready_only_for_this_kit_with_an_engine(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write(root / 'kit/kit.json', json.dumps(dict(kit_id='k2')))
            state = root / 'state'
            self.assertFalse(gb.status(root / 'kit', state)['ready'])
            write(state / 'Build/build-state.json', json.dumps(dict(ok=True, kit_id='k1')))
            write(state / 'Game/Fable II.bundle/Contents/MacOS/fable_2', 'bin')
            self.assertFalse(gb.status(root / 'kit', state)['ready'])  # built from an older kit: rebuild
            write(state / 'Build/build-state.json', json.dumps(dict(ok=True, kit_id='k2')))
            self.assertTrue(gb.status(root / 'kit', state)['ready'])


class RecompPatchPoolTests(unittest.TestCase):
    def test_pool_matches_one_process(self):
        import apply_recomp_patches as patches
        unit = ('DEFINE_REX_FUNC(sub_82000000) {\n\t// lwsync\n\tREX_ENTER_GLOBAL_LOCK();\nloc_82000004:\n'
                '\t// lwarx r11,0,r3\n\t// stwcx. r10,0,r3\n\tREX_LEAVE_GLOBAL_LOCK();\n\t// sync\n}\n')
        results = []
        for jobs in ('1', '4'):
            os.environ['FABLE2_RECOMP_PATCH_JOBS'] = jobs
            with tempfile.TemporaryDirectory() as temporary:
                paths = []
                for i in range(6):
                    paths.append(Path(temporary) / ('fable_2_recomp.%d.cpp' % i))
                    write(paths[-1], unit * (i + 1))
                pool = patches._pool()
                self.assertEqual(pool is None, jobs == '1')
                try:
                    pool_map = pool.map if pool else map
                    counts = (patches.apply_guest_barriers(paths, pool_map), patches.apply_fast_msr(paths, pool_map))
                finally:
                    if pool:
                        pool.close()
                        pool.join()
                results.append((counts, [p.read_text() for p in paths]))
        os.environ.pop('FABLE2_RECOMP_PATCH_JOBS', None)
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0][0], (42, 21))  # 2 fences and 1 atomic section per unit, 21 units
        self.assertIn('FABLE2_GUEST_LWSYNC', results[0][1][0])


class ReleaseCheckTests(unittest.TestCase):
    def test_forbidden_names(self):
        import fable2_release as release
        bad = ['Contents/MacOS/fable_2', 'Contents/Resources/kit/game-src/generated/default/a.cpp',
               'Contents/Resources/native-shaders/x', 'Contents/Resources/kit/x.metallib', 'Contents/default.xex',
               'Contents/Resources/kit/sdk/lib/x.o']
        good = ['Contents/Resources/kit/game-src/fable_2_manifest.toml', 'Contents/Resources/kit/game-src/src/core/a.cpp',
                'Contents/Resources/kit/sdk/lib/cmake/rexglue/rexglueConfig.cmake']
        for rel in bad:
            self.assertTrue(release.FORBIDDEN.search(rel.split('Contents/Resources/kit/', 1)[-1]), rel)
        for rel in good:
            self.assertFalse(release.FORBIDDEN.search(rel.split('Contents/Resources/kit/', 1)[-1]), rel)



class ReleaseZipTests(unittest.TestCase):
    """macOS 27 called a downloaded release "damaged" when its zip carried __MACOSX/._* entries (--sequesterRsrc);
    without them it shows "Not Opened" and Open Anyway works (docs/release-app.md "Gatekeeper")."""

    def test_zip_has_no_appledouble_entries(self):
        import fable2_release as release
        import fable2_portable as portable
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / 'Test.app'
            write(app / 'Contents/MacOS/tool', '#!/bin/sh\n')
            write(app / 'Contents/Resources/a.txt', 'a')
            for path in (app, app / 'Contents/MacOS/tool', app / 'Contents/Resources/a.txt'):
                subprocess.run(['xattr', '-w', 'com.example.test', 'x', str(path)], check=True)
            sequestered = root / 'old.zip'
            subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', str(app), str(sequestered)],
                           check=True)
            self.assertTrue(portable.metadata_entries(sequestered))  # the check sees what the old zip shipped
            with self.assertRaises(RuntimeError):
                portable.check_zip(sequestered)
            fixed = root / 'new.zip'
            release.zip_app(app, fixed)
            self.assertEqual(portable.metadata_entries(fixed), [])

    def test_built_release_zip(self):
        import fable2_portable as portable
        zip_path = ROOT / 'out/release/Fable II.zip'
        if not zip_path.exists(): self.skipTest('no out/release/Fable II.zip')
        self.assertEqual(portable.metadata_entries(zip_path), [])

    def test_no_unsealed_signing(self):
        import fable2_release as release
        self.assertEqual(release.SIGNING_MODES[0], 'sealed')
        self.assertNotIn('unsealed', release.SIGNING_MODES)


class RecipeTests(unittest.TestCase):
    def test_recipe_carries_no_microcode(self):
        """The shader recipe (--shader-recipe) holds locations, hashes and patch words only."""
        import fable2_shader_bank as bank
        allowed = {'hash', 'type', 'dwords', 'sha256', 'bank', 'entry', 'offset', 'patches'}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = bytes(range(64)) * 4
            ucode = entry[16:48]
            xsh = root / 'list.xsh'
            import struct
            xsh.write_bytes(struct.pack('<II', bank.XSH_MAGIC, int.from_bytes(struct.pack('>I', bank.XSH_VERSION), 'little'))
                            + struct.pack('<QI', 0x1234, (len(ucode) // 4) | (1 << 31)) + ucode)
            original = bank.load_banks
            bank.load_banks = lambda _root: {'ShadersRelease.sbk': [entry]}
            original_root = bank.data_root
            bank.data_root = lambda _arg=None: root
            (root / 'data/Shaders').mkdir(parents=True)
            (root / 'data/Shaders/ShadersRelease.sbk').write_bytes(b'x')
            try:
                recipe = bank.recipe(xsh, root=None, log=lambda *a: None)
            finally:
                bank.load_banks, bank.data_root = original, original_root
            self.assertEqual(len(recipe['shaders']), 1)
            self.assertLessEqual(set(recipe['shaders'][0]), allowed)
            self.assertNotIn(ucode.hex(), json.dumps(recipe))


if __name__ == '__main__':
    unittest.main()
