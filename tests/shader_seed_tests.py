"""Offline shader seed (docs/offline-shader-seed.md): the shader-bank reader, recipe and rebuild on a synthetic bank
(no game data), the kit's end-to-end auto rule, and the portable seed-store. Run: /usr/bin/python3 tests/shader_seed_tests.py
With a built FableIIPrepare at $FABLE2_PREPARE, also checks that the Swift rebuild writes the same .xsh."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import fable2_shader_bank as sb  # noqa: E402


def make_bank(path, entries):
    """A ShaderBankFile like the game's: header, filler, count + size table + offset table, then per entry
    (u32 0, u32 compressed size, zlib stream without a final block)."""
    blobs = []
    for e in entries:
        c = zlib.compressobj(9)
        blobs.append(c.compress(e) + c.flush(zlib.Z_SYNC_FLUSH))  # no final block, as in the game's banks
    head = b'ShaderBankFile\0\0' + b'\0' * 33
    table = struct.pack('>I', len(entries)) + b''.join(struct.pack('>I', len(e)) for e in entries)
    offsets, at = [], 0
    for b in blobs:
        offsets.append(at)
        at += 8 + len(b)
    table += b''.join(struct.pack('>I', o) for o in offsets)
    body = b''.join(struct.pack('>II', 0, len(b)) + b for b in blobs)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(head + table + body)


def make_xsh(path, shaders):
    out = struct.pack('<II', sb.XSH_MAGIC, int.from_bytes(struct.pack('>I', sb.XSH_VERSION), 'little'))
    for h, kind, ucode in shaders:
        out += struct.pack('<QI', h, len(ucode) // 4 | (kind << 31)) + ucode
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(out)


def dwords(*values):
    return b''.join(struct.pack('>I', v) for v in values)


class BankTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.data = self.tmp / 'game'
        ps = dwords(*range(0x10000, 0x10010))           # 16 dwords, exact
        vs = dwords(*range(0x20000, 0x20014))           # 20 dwords, two of them patched at "bind time"
        self.entries = [b'\x10\x2a\x11\x01' + b'\0' * 12 + ps + b'\0' * 8, b'\x10\x2a\x11\x01' + b'\0' * 4 + vs]
        make_bank(self.data / 'data/Shaders/ShadersRelease.sbk', self.entries)
        patched = bytearray(vs)
        struct.pack_into('>I', patched, 4 * 5, 0xDEADBEEF)
        struct.pack_into('>I', patched, 4 * 6, 0xFEEDF00D)
        missing = dwords(*range(0x30000, 0x30008))
        self.shaders = [(0x1111, 1, ps), (0x2222, 0, bytes(patched)), (0x3333, 0, missing)]
        make_xsh(self.tmp / 'in/4D5307F1.xsh', self.shaders)
        (self.tmp / 'in/4D5307F1.metal.xpso').write_bytes(b'XEPS-test-pipelines')

    def test_read_bank(self):
        self.assertEqual(sb.read_bank(self.data / 'data/Shaders/ShadersRelease.sbk'), self.entries)

    def test_recipe_and_rebuild(self):
        r = sb.recipe(self.tmp / 'in/4D5307F1.xsh', self.tmp / 'in/4D5307F1.metal.xpso', self.data)
        self.assertEqual(r['stats'].get('pixel_exact'), 1)
        self.assertEqual(r['stats'].get('vertex_patched'), 1)
        self.assertEqual(r['unresolved'], ['0000000000003333'])
        vs = [s for s in r['shaders'] if s['hash'] == '0000000000002222'][0]
        self.assertEqual(vs['patches'], [[5, 0xDEADBEEF], [6, 0xFEEDF00D]])
        self.assertNotIn('ucode', json.dumps(r))
        path = self.tmp / 'recipe.json'
        path.write_text(json.dumps(r))
        out = sb.rebuild(path, self.tmp / 'cache', self.data)
        self.assertEqual((out['shaders'], out['mismatched']), (2, []))
        rebuilt = sb.read_xsh(self.tmp / 'cache/shaders/shareable/4D5307F1.xsh')
        self.assertEqual(rebuilt, self.shaders[:2])
        self.assertEqual((self.tmp / 'cache/shaders/shareable/4D5307F1.metal.xpso').read_bytes(), b'XEPS-test-pipelines')
        prepare = os.environ.get('FABLE2_PREPARE')
        if prepare:
            subprocess.run([prepare, '--target', str(self.data), '--shader-lists', str(path), '--cache',
                            str(self.tmp / 'swift')], check=True, stdout=subprocess.DEVNULL)
            self.assertEqual((self.tmp / 'swift/shaders/shareable/4D5307F1.xsh').read_bytes(),
                             (self.tmp / 'cache/shaders/shareable/4D5307F1.xsh').read_bytes())

    def test_rebuild_detects_other_disc(self):
        r = sb.recipe(self.tmp / 'in/4D5307F1.xsh', None, self.data)
        path = self.tmp / 'recipe.json'
        path.write_text(json.dumps(r))
        other = self.tmp / 'other'
        make_bank(other / 'data/Shaders/ShadersRelease.sbk', [e[::-1] for e in self.entries])
        out = sb.rebuild(path, self.tmp / 'cache2', other)
        self.assertEqual(out['shaders'], 0)
        self.assertEqual(len(out['mismatched']), 2)


class KitE2ETests(unittest.TestCase):
    def test_auto_rule(self):
        import fable2_benchmark_kit as kit
        with tempfile.TemporaryDirectory() as tmp:
            saved = kit.E2E_STATE
            kit.E2E_STATE = Path(tmp) / 'e2e-state.json'
            try:
                self.assertTrue(kit.e2e_needed()[0])
                kit.E2E_STATE.write_text(json.dumps(dict(inputs=kit.e2e_inputs_hash(), git_sha='abc', when='now')))
                self.assertFalse(kit.e2e_needed()[0])
                kit.E2E_STATE.write_text(json.dumps(dict(inputs='0' * 16, git_sha='abc', when='now')))
                self.assertTrue(kit.e2e_needed()[0])
            finally:
                kit.E2E_STATE = saved


if __name__ == '__main__':
    unittest.main()
