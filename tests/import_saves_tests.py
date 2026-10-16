"""Release app: Import Saves (2026-10-15). The release app has its own state folder, so it starts without the saves
of a portable/dev install. `--import-saves FOLDER [--replace]` runs the same SaveImport the window uses
(tools/macos/Player/main.swift). Stand-in folders only (never the real Application Support). Checks: copies
(source untouched), keeps an existing save unless --replace, backs up a replaced one, copies profile data only when
missing, and finds another install through FABLE2_IMPORT_SAVES_FROM. Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = [ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift')) \
    + sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))
PROFILE = 'B13EBABEBABEBABE'


def hero(saves, name, text):
    container = Path(saves) / PROFILE / '4D5307F1'
    (container / '00000001' / name).mkdir(parents=True, exist_ok=True)
    (container / '00000001' / name / 'mainsave.bin').write_text(text)
    (container / 'Headers/00000001').mkdir(parents=True, exist_ok=True)
    (container / 'Headers/00000001' / (name + '.header')).write_text(text)


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class ImportSavesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='import-saves-')
        root = Path(cls.temp.name)
        app = root / 'Release.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o',
                        str(app / 'Contents/MacOS/FableIIPlayer')] + [str(s) for s in APP], check=True, capture_output=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FableIIPlayer', CFBundleIdentifier='org.fable2recomp.test.release', CFBundlePackageType='APPL',
            Fable2BuildsGame=True, Fable2StateFolder='Fable II test release')))
        cls.binary = app / 'Contents/MacOS/FableIIPlayer'

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_import(self, state, source, *extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(state), FABLE2_IMPORT_SAVES_FROM=str(source))
        r = subprocess.run([str(self.binary), '--import-saves', str(source)] + list(extra), env=env,
                           capture_output=True, text=True, timeout=60)
        return json.loads(r.stdout)

    def test_copies_keeps_and_replaces(self):
        base = Path(tempfile.mkdtemp(dir=self.temp.name))
        other = base / 'other-install'  # a state folder (contains saves/)
        hero(other / 'saves', 'Hero000', 'theirs-0')
        hero(other / 'saves', 'Hero004', 'theirs-4')
        (other / 'saves' / PROFILE / '4D5307F1/profile/User').mkdir(parents=True)
        (other / 'saves' / PROFILE / '4D5307F1/profile/User/63E83FFF').write_text('options')
        state = base / 'release-state'
        hero(state / 'saves', 'Hero000', 'mine-0')

        out = self.run_import(state, other)
        self.assertTrue(out['ok'])
        self.assertEqual(out['imported'], [PROFILE + '/4D5307F1/00000001/Hero004'])
        self.assertEqual(out['kept'], [PROFILE + '/4D5307F1/00000001/Hero000'])
        self.assertEqual(out['other_installs'], [str(other / 'saves')])
        dest = state / 'saves' / PROFILE / '4D5307F1'
        self.assertEqual((dest / '00000001/Hero000/mainsave.bin').read_text(), 'mine-0')  # never overwritten unasked
        self.assertEqual((dest / '00000001/Hero004/mainsave.bin').read_text(), 'theirs-4')
        self.assertEqual((dest / 'Headers/00000001/Hero004.header').read_text(), 'theirs-4')
        self.assertEqual((dest / 'profile/User/63E83FFF').read_text(), 'options')
        self.assertTrue((other / 'saves' / PROFILE / '4D5307F1/00000001/Hero004/mainsave.bin').is_file())  # copied

        out = self.run_import(state, other / 'saves', '--replace')  # a saves folder works too
        self.assertEqual(out['replaced'], [PROFILE + '/4D5307F1/00000001/Hero000', PROFILE + '/4D5307F1/00000001/Hero004'])
        self.assertEqual((dest / '00000001/Hero000/mainsave.bin').read_text(), 'theirs-0')
        backup = Path(out['backup'])
        self.assertEqual((backup / PROFILE / '4D5307F1/00000001/Hero000/mainsave.bin').read_text(), 'mine-0')
        self.assertEqual((other / 'saves' / PROFILE / '4D5307F1/00000001/Hero000/mainsave.bin').read_text(), 'theirs-0')

    def test_folder_without_saves(self):
        base = Path(tempfile.mkdtemp(dir=self.temp.name))
        (base / 'empty').mkdir()
        out = self.run_import(base / 'state', base / 'empty')
        self.assertEqual((out['imported'], out['other_installs']), ([], []))


if __name__ == '__main__':
    unittest.main()
