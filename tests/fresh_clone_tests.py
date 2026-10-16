"""Fresh-clone fixes (2026-10-15, M3 Pro build from source):
  - skip intro: a checkout had no out/skipintro stand-ins, so "Skip intro movies" played the logos anyway.
    tools/fable2_skip_intro.py makes them (same trim as FableIIPrepare's trimBink);
  - save library: title-screen play (the only choice with no saves yet) never reached the library. The title-play
    watcher (fable2_dev_session.watch_live) copies each live save into it.
Temp folders only; never the real library or live state. Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fable2_skip_intro as skip  # noqa: E402
import fable2_dev_session as dev  # noqa: E402
import fable2_test_control as control  # noqa: E402


def bink(frames, sizes):
    """A minimal Bink 1 file: header, no audio tracks, frame index, frame bodies."""
    header_end = 0x2C
    table = header_end + (frames + 1) * 4
    offsets, at = [], table
    for size in sizes:
        offsets.append(at)
        at += size
    offsets.append(at)
    head = bytearray(b'BIKi' + bytes(0x28))
    struct.pack_into('<IIII', head, 4, at - 8, frames, max(sizes), frames)
    index = [offsets[0] | 1] + offsets[1:]  # keyframe bit on frame 0
    return bytes(head) + struct.pack('<%dI' % len(index), *index) + b''.join(bytes([i]) * s for i, s in enumerate(sizes))


class SkipIntroTests(unittest.TestCase):
    def test_trim_keeps_first_frames(self):
        out = skip.trim_bink(bink(4, [10, 20, 30, 40]), 2)
        self.assertEqual(struct.unpack_from('<I', out, 8)[0], 2)
        self.assertEqual(struct.unpack_from('<I', out, 12)[0], 20)  # largest kept frame
        self.assertEqual(len(out), 0x2C + 3 * 4 + 30)
        self.assertEqual(struct.unpack_from('<I', out, 4)[0], len(out) - 8)
        self.assertEqual(struct.unpack_from('<I', out, 0x2C)[0] & 1, 1)  # keyframe bit kept

    def test_ensure_makes_missing_stand_ins_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            videos = root / 'data/art/videos'
            videos.mkdir(parents=True)
            for name in ('Microsoft_Logo.bik', 'lionhead_logo.bik', 'middlewarelogos.bik'):  # disc case varies
                (videos / name).write_bytes(bink(4, [10, 20, 30, 40]))
            self.assertEqual(skip.status(root)['missing'], ['ms.bik', 'lh.bik', 'mw.bik'])  # a fresh clone
            self.assertEqual(skip.ensure(root)['made'], ['ms.bik', 'lh.bik', 'mw.bik'])
            self.assertEqual(skip.ensure(root)['made'], [])
            self.assertEqual(skip.status(root)['missing'], [])

    def test_ensure_without_videos_never_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = skip.ensure(Path(tmp))
            self.assertEqual(result['made'], [])
            self.assertEqual(len(result['sources_missing']), 3)

    def test_launch_and_playtest_and_build_ensure_them(self):
        self.assertIn('skip_intro_movies.ensure(data_root)', (ROOT / 'tools/fable2_dev_session.py').read_text())
        self.assertIn('fable2_skip_intro.ensure(ROOT)', (ROOT / 'tools/fable2_playtest.py').read_text())
        self.assertIn('fable2_skip_intro.ensure(ROOT', (ROOT / 'tools/build_macos_metal.py').read_text())


def write_save(state, hero, quest='QC060_NewBeginning'):
    slot = state / control.HERO_SLOTS / hero
    slot.mkdir(parents=True, exist_ok=True)
    (slot / 'mainsave.bin').write_bytes(os.urandom(64))
    xml = '<QuestName type="s">%s</QuestName><LevelName type="s">BowerLake</LevelName>' % quest
    (slot / 'chaptersave.bin').write_bytes(b'\0\0\0\0' + xml.encode())
    headers = state / control.HERO_HEADERS
    headers.mkdir(parents=True, exist_ok=True)
    raw = bytearray(0x138)
    name = 'Hero 1'.encode('utf-16-be')
    raw[8:8 + len(name)] = name
    raw[0x108:0x108 + len(hero)] = hero.encode()
    (headers / (hero + '.header')).write_bytes(bytes(raw))


class TitleCaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.state = Path(self.tmp.name) / 'live'
        self.library = Path(self.tmp.name) / 'library'
        self.saved = (dev.LIBRARY, dev.STABLE_SECONDS, dev.POLL_SECONDS)
        dev.LIBRARY, dev.STABLE_SECONDS, dev.POLL_SECONDS = self.library, 0.3, 0.05

    def tearDown(self):
        dev.LIBRARY, dev.STABLE_SECONDS, dev.POLL_SECONDS = self.saved
        self.tmp.cleanup()

    def test_live_save_goes_to_the_library(self):
        game = subprocess.Popen(['sleep', '30'])  # stands in for the game's pid
        try:
            write_save(self.state, 'Hero000', 'QC010_Childhood')  # existing save: not captured
            watcher = threading.Thread(target=dev.watch_live, args=(game.pid, self.state), daemon=True)
            watcher.start()
            time.sleep(0.3)
            write_save(self.state, 'Hero000', 'QC075_JourneyToRavenscar')  # the game saves
            deadline = time.time() + 10
            while time.time() < deadline and not list(self.library.glob('*/save.json')): time.sleep(0.1)
        finally:
            game.kill()
            game.wait()
        watcher.join(10)
        self.assertFalse(watcher.is_alive(), 'watcher stops when the game exits')
        entries = list(self.library.glob('*/save.json'))
        self.assertEqual(len(entries), 1)
        meta = json.loads(entries[0].read_text())
        self.assertTrue(meta['name'].startswith('live-hero000_'))
        self.assertEqual(meta['QuestName'], 'QC075_JourneyToRavenscar')
        self.assertIsNone(meta['written_back_to'])
        self.assertEqual(meta['origin']['kind'], 'title')
        self.assertTrue((entries[0].parent / 'Hero000/mainsave.bin').is_file())
        self.assertTrue((entries[0].parent / 'Hero000.header').is_file())

    def test_title_launch_starts_the_watcher(self):
        text = (ROOT / 'tools/fable2_dev_session.py').read_text()
        self.assertIn("'watch-live', '--pid', str(pid)", text)


if __name__ == '__main__':
    unittest.main()
