"""macOS privacy folders, App Translocation and Move to Applications in the release app
(tools/macos/Shared/AppAccess.swift, tools/macos/Player/main.swift; docs/release-app.md "Protected folders and Move to
Applications").

Builds the player and the extractor with swiftc into a stand-in release .app (Fable2BuildsGame, a stand-in build kit)
in a scratch folder and drives:
  - the pure classification (--access-classify: protected folders, errno and helper output, translocation by path);
  - --access-check / --bundle-location on real scratch paths;
  - the Move to Applications copy (--move-to-applications; scratch Applications and Trash folders, never the real ones);
  - the extractor reading an image handed over as an open file (--iso-fd) from a folder it cannot open;
  - the build driver reporting a refused read as "[denied] <errno> <path>";
  - the real windows in background test mode: a disc image and a build kit in a chmod-ed stand-in "Downloads"
    folder show the refused-access state, Try Again checks again (still refused), and after the folder is readable
    Try Again goes on (extraction starts; the build runs the kit copied out of the app).
A real macOS privacy prompt cannot be produced in automation (manual check in docs/release-app.md). Runs under
/usr/bin/python3 (3.9). Never touches the real Application Support, Applications, Trash or Downloads."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLAYER_SOURCES = ([ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift'))
                  + sorted((ROOT / 'tools/macos/Shared').glob('*.swift')))
PREPARE_SOURCES = [ROOT / p for p in ('tools/macos/Prepare/main.swift', 'tools/macos/Prepare/ShaderLists.swift',
                                      'tools/macos/Setup/GDFX.swift', 'tools/macos/Setup/Support.swift')]
BUNDLE_ID = 'org.fable2recomp.metal.release-access-test'
KIT_ID = 'kit-access-test'
# Stand-in build driver: the first run reports a refused read (as the real driver does for a PermissionError), later
# runs a plain failure, so the test sees Try Again run python again from the kit copied out of the app.
STANDIN_DRIVER = r'''import os, sys
kit = sys.argv[sys.argv.index('--kit') + 1]
state = sys.argv[sys.argv.index('--state') + 1]
print('[stage] cmake stand-in driver running from ' + kit, flush=True)
marker = os.path.join(state, 'standin-ran-once')
if not os.path.exists(marker):
    open(marker, 'w').close()
    print('[denied] 1 ' + os.environ['STANDIN_DENIED_PATH'])
    print('[error] macOS did not allow reading it.')
    sys.exit(1)
print('[error] stand-in driver: second run')
sys.exit(1)
'''


def wait_for(predicate, timeout, what, logs=None):
    """Polls a readiness condition; the timeout is only a guard. `logs` (a folder) is shown on a timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(0.1)
    detail = ''
    for path in sorted(Path(logs).glob('*.log')) if logs else []:
        detail += '\n--- %s ---\n%s' % (path.name, path.read_text()[-3000:])
    raise AssertionError('timed out waiting for ' + what + detail)


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class ReleaseAccessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='release-access-')
        cls.root = Path(cls.temp.name).resolve()
        cls.bin = cls.root / 'bin'
        cls.bin.mkdir()
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o', str(cls.bin / 'FableIIPlayer')]
                       + [str(s) for s in PLAYER_SOURCES], check=True, capture_output=True)
        subprocess.run(['xcrun', 'swiftc', '-O', '-parse-as-library', '-target', 'arm64-apple-macos15.0',
                        '-o', str(cls.bin / 'FableIIPrepare')] + [str(s) for s in PREPARE_SOURCES],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        for path, dirs, _files in os.walk(str(cls.root)):
            for d in dirs:
                try:
                    os.chmod(os.path.join(path, d), 0o755)
                except OSError:
                    pass
        cls.temp.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix='case-', dir=str(self.root)))
        self.downloads = self.case / 'Downloads'  # stand-in for ~/Downloads (FABLE2_TEST_PROTECTED_DIRS)
        self.downloads.mkdir()
        self.state = self.case / 'state'
        self.locked = []
        self.procs = []

    def tearDown(self):
        for path in self.locked: os.chmod(str(path), 0o755)
        for proc in self.procs:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

    def lock(self, path):
        os.chmod(str(path), 0)
        self.locked.append(path)

    def unlock(self, path):
        os.chmod(str(path), 0o755)

    def make_app(self, folder):
        app = folder / 'Fable II.app'
        macos = app / 'Contents/MacOS'
        macos.mkdir(parents=True)
        for name in ('FableIIPlayer', 'FableIIPrepare'): shutil.copy2(str(self.bin / name), str(macos / name))
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FableIIPlayer', CFBundleIdentifier=BUNDLE_ID, CFBundlePackageType='APPL',
            Fable2BuildsGame=True, Fable2StateFolder='Fable II access test', Fable2KitID=KIT_ID)))
        kit = app / 'Contents/Resources/kit'
        (kit / 'game-src/tools').mkdir(parents=True)
        (kit / 'kit.json').write_text(json.dumps({'kit_id': KIT_ID}))
        (kit / 'game-src/tools/fable2_game_build.py').write_text(STANDIN_DRIVER)
        return app

    def env(self, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(self.state), FABLE2_PORTABLE_REUSE_FROM='',
                   FABLE2_TEST_PROTECTED_DIRS=str(self.downloads), FABLE2_TEST_BACKGROUND='1',
                   FABLE2_PLAYER_SHADER_PREP='0', FABLE2_TEST_APPLICATIONS_DIR=str(self.case / 'Applications'),
                   FABLE2_TEST_TRASH_DIR=str(self.case / 'Trash'))
        env.update(extra)
        return env

    def cli(self, *args, stdin=None, binary=None, **env):
        done = subprocess.run([str(binary or self.bin / 'FableIIPlayer'), *args], env=self.env(**env), input=stdin,
                              capture_output=True, text=True, timeout=60)
        return json.loads(done.stdout)

    # --- Pure classification ----------------------------------------------------------------------------

    def classify(self, cases):
        return self.cli('--access-classify', stdin=json.dumps(cases))

    def test_protected_folders(self):
        home = '/Users/Shared/tester-home'
        rows = self.classify([dict(path=home + '/' + p, home=home) for p in (
            'Downloads/Fable II.app', 'Desktop/x.iso', 'Documents/Games/x.iso', 'Library/Mobile Documents/com~apple~CloudDocs/x.iso',
            'Library/CloudStorage/Dropbox/x.iso', 'Downloadsfoo/x.iso', 'Library/Application Support/Fable II for Mac/x')]
            + [dict(path='/Volumes/USB/x.iso', home=home), dict(path='/Applications/Fable II.app', home=home)])
        self.assertEqual([r['protectedFolder'] for r in rows],
                         ['Downloads', 'Desktop', 'Documents', 'iCloud Drive', 'cloud storage', None, None,
                          'an external or network volume', None])

    def test_errno_classification(self):
        home = '/Users/Shared/tester-home'
        rows = self.classify([dict(path=home + '/Downloads/a.iso', home=home, errno=1),   # EPERM: macOS privacy
                              dict(path=home + '/Desktop/a.iso', home=home, errno=13),    # EACCES there too
                              dict(path='/tmp/a.iso', home=home, errno=13),               # plain permissions
                              dict(path='/tmp/a.iso', home=home, errno=1),
                              dict(path='/tmp/a.iso', home=home, errno=2),
                              dict(path='/tmp/a.iso', home=home, errno=5),
                              dict(path='/tmp/a.iso', home=home, errno=0)])
        self.assertEqual(rows[0]['problem'], dict(kind='privacy', folder='Downloads', path=home + '/Downloads/a.iso'))
        self.assertEqual(rows[1]['problem']['kind'], 'privacy')
        self.assertEqual([r['problem'] and r['problem']['kind'] for r in rows[2:]],
                         ['permissions', 'permissions', 'missing', 'other', None])

    def test_helper_output_classification(self):
        home = '/Users/Shared/tester-home'
        app = home + '/Downloads/Fable II.app/Contents/Resources/kit/game-src/tools/fable2_game_build.py'
        rows = self.classify([
            # The failure the maintainer hit: python3 could not open the driver inside the app in Downloads.
            dict(path='', home=home, output="/usr/bin/python3: can't open file '%s': [Errno 1] Operation not permitted" % app),
            dict(path='', home=home, output="PermissionError: [Errno 1] Operation not permitted: '%s/Desktop/k.json'" % home),
            dict(path='', home=home, output='$ cmake\n[denied] 13 /opt/x/kit.json\n[error] macOS did not allow it'),
            dict(path='', home=home, output='[error] Compiling the game failed (make exit 2).'),
        ])
        self.assertEqual(rows[0]['outputProblem'], dict(kind='privacy', folder='Downloads', path=app))
        self.assertEqual(rows[1]['outputProblem']['folder'], 'Desktop')
        self.assertEqual(rows[2]['outputProblem'], dict(kind='permissions', folder=None, path='/opt/x/kit.json'))
        self.assertIsNone(rows[3]['outputProblem'])

    def test_translocation_and_move_offer(self):
        home = '/Users/Shared/tester-home'
        rows = self.classify([
            dict(path='/private/var/folders/ab/xyz/T/AppTranslocation/0A1B2C3D/d/Fable II.app', home=home),
            dict(path=home + '/Downloads/Fable II.app', home=home),
            dict(path='/Volumes/Fable II/Fable II.app', home=home, readOnly=True),
            dict(path='/Applications/Fable II.app', home=home),
            dict(path='/Users/Shared/tester-home/Games/Fable II.app', home=home),
        ])
        self.assertEqual([r['translocated'] for r in rows], [True, False, False, False, False])
        self.assertEqual([r['shouldOfferMove'] for r in rows], [True, True, True, False, False])
        self.assertIn('temporary copy', rows[0]['reason'])
        self.assertIn('Downloads folder', rows[1]['reason'])

    # --- Real paths ----------------------------------------------------------------------------------------

    def test_access_check(self):
        iso = self.downloads / 'locked/fable.iso'
        iso.parent.mkdir()
        iso.write_bytes(b'x')
        self.assertTrue(self.cli('--access-check', str(iso))['ok'])
        self.lock(iso.parent)
        result = self.cli('--access-check', str(iso))
        self.assertFalse(result['ok'])
        self.assertEqual(result['problem'], dict(kind='privacy', folder='Downloads', path=str(iso)))
        other = self.case / 'other/fable.iso'
        other.parent.mkdir()
        other.write_bytes(b'x')
        self.lock(other.parent)
        self.assertEqual(self.cli('--access-check', str(other))['problem']['kind'], 'permissions')
        self.assertEqual(self.cli('--access-check', str(self.case / 'gone.iso'))['problem']['kind'], 'missing')

    def test_bundle_location_of_the_running_app(self):
        app = self.make_app(self.downloads)
        location = self.cli('--bundle-location', binary=app / 'Contents/MacOS/FableIIPlayer')
        self.assertEqual(location['bundlePath'], str(app))
        self.assertFalse(location['translocated'])
        self.assertEqual(location['folder'], 'Downloads')
        self.assertTrue(location['shouldOfferMove'])
        elsewhere = self.make_app(self.case / 'Games')
        self.assertFalse(self.cli('--bundle-location', binary=elsewhere / 'Contents/MacOS/FableIIPlayer')['shouldOfferMove'])

    def test_move_to_applications(self):
        app = self.make_app(self.downloads)
        # A quarantined data file stands in for the download quarantine (never on the executable or the bundle: running
        # a quarantined binary would make macOS show its Gatekeeper alert).
        note = app / 'Contents/Resources/note.txt'
        note.write_text('x')
        subprocess.run(['xattr', '-w', 'com.apple.quarantine', '0083;00000000;Safari;', str(note)], check=True)
        result = self.cli('--move-to-applications', binary=app / 'Contents/MacOS/FableIIPlayer')
        moved = self.case / 'Applications/Fable II.app'
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['destination'], str(moved))
        self.assertTrue(result['trashedOriginal'])
        self.assertFalse(app.exists())
        self.assertTrue((self.case / 'Trash/Fable II.app').is_dir())
        self.assertTrue((moved / 'Contents/MacOS/FableIIPlayer').is_file())
        xattrs = subprocess.run(['xattr', '-r', str(moved)], capture_output=True, text=True).stdout
        self.assertNotIn('com.apple.quarantine', xattrs)
        # From Applications nothing is offered; moving again over an older copy puts that one in the Trash.
        self.assertFalse(self.cli('--bundle-location', binary=moved / 'Contents/MacOS/FableIIPlayer',
                                  FABLE2_TEST_APPLICATIONS_DIR='')['shouldOfferMove'])
        shutil.rmtree(str(self.case / 'Trash'))
        again = self.make_app(self.downloads)
        result = self.cli('--move-to-applications', binary=again / 'Contents/MacOS/FableIIPlayer')
        self.assertTrue(result['ok'] and result['replacedExisting'], result)

    # --- Extractor and build driver --------------------------------------------------------------------

    def test_extractor_reads_the_handed_over_image(self):
        iso = self.downloads / 'locked/fable.iso'
        iso.parent.mkdir()
        iso.write_bytes(b'\0' * 70000)  # opens, but holds no disc header
        fd = os.open(str(iso), os.O_RDONLY)
        self.lock(iso.parent)  # the extractor itself cannot open the path any more
        target = self.case / 'GameData'
        try:
            done = subprocess.run([str(self.bin / 'FableIIPrepare'), '--target', str(target), '--iso', str(iso),
                                   '--iso-fd', str(fd), '--headless'], pass_fds=(fd,), capture_output=True, text=True,
                                  timeout=60)
        finally:
            os.close(fd)
        output = done.stdout + done.stderr
        self.assertIn('no GDFX header', output)  # read through the open file, not refused at open()
        self.assertNotIn('Cannot open', output)
        done = subprocess.run([str(self.bin / 'FableIIPrepare'), '--target', str(target), '--iso', str(iso), '--headless'],
                              capture_output=True, text=True, timeout=60)
        self.assertIn('permission', done.stdout + done.stderr)  # without the handed-over file it is refused

    def test_build_driver_reports_a_refused_read(self):
        kit = self.case / 'kit'
        kit.mkdir()
        (kit / 'kit.json').write_text('{}')
        os.chmod(str(kit / 'kit.json'), 0)
        try:
            done = subprocess.run(['/usr/bin/python3', '-I', '-B', str(ROOT / 'tools/fable2_game_build.py'), 'build',
                                   '--kit', str(kit), '--state', str(self.state), '--game-data', str(self.case)],
                                  capture_output=True, text=True, timeout=60)
        finally:
            os.chmod(str(kit / 'kit.json'), 0o644)
        self.assertEqual(done.returncode, 1)
        self.assertIn('[denied] 13 %s' % (kit / 'kit.json'), done.stdout)

    # --- Windows (background test mode) -----------------------------------------------------------------

    def launch(self, app_binary, *args, **env):
        with open(str(self.case / 'player-stdout.log'), 'a') as log:
            proc = subprocess.Popen([str(app_binary), *args], env=self.env(**env), stdout=log, stderr=log)
        self.procs.append(proc)
        return proc

    def log_text(self, name):
        path = self.state / 'logs' / name
        return path.read_text() if path.exists() else ''

    def test_denied_disc_image_and_try_again(self):
        app = self.make_app(self.case / 'Apps')
        iso = self.downloads / 'locked/fable.iso'
        iso.parent.mkdir()
        iso.write_bytes(b'\0' * 70000)
        self.lock(iso.parent)
        retry = self.case / 'retry'
        self.launch(app / 'Contents/MacOS/FableIIPlayer', '--iso', str(iso), FABLE2_TEST_RETRY_WHEN=str(retry))
        wait_for(lambda: '[access] denied (disc image): privacy %s' % iso in self.log_text('player.log'), 60,
                 'the refused disc image')
        retry.touch()  # Try Again while still refused: checked again, still refused, and said so
        wait_for(lambda: '[access] still denied (disc image)' in self.log_text('player.log'), 60, 'still denied')
        self.unlock(iso.parent)
        retry.touch()  # Try Again after access was given: the extraction starts
        wait_for(lambda: 'no GDFX header' in self.log_text('prepare.log'), 60, 'the extractor reading the image')
        self.assertIn('[player] extracting the game files from %s' % iso, self.log_text('prepare.log'))

    def test_denied_build_kit_and_try_again(self):
        app = self.make_app(self.downloads)  # the app itself in "Downloads", as in the maintainer's report
        kit = app / 'Contents/Resources/kit'
        game = self.state / 'GameData'
        (game / 'data').mkdir(parents=True)
        (game / 'default.xex').write_text('xex')
        (game / '.fable2-portable.json').write_text(json.dumps({'complete': True}))
        tools_ready = self.case / 'clt-installed'
        tools_ready.touch()
        # "Don't ask again" was chosen on the Move offer: the build starts in place.
        self.state.mkdir(exist_ok=True)
        (self.state / 'player-settings.plist').write_bytes(plistlib.dumps({'moveOfferDeclined': True}))
        self.lock(kit)
        retry = self.case / 'retry'
        denied_path = str(self.downloads / 'elsewhere/thing.json')
        self.launch(app / 'Contents/MacOS/FableIIPlayer', FABLE2_TEST_RETRY_WHEN=str(retry),
                    FABLE2_SIMULATE_NO_CLT=str(tools_ready), STANDIN_DENIED_PATH=denied_path)
        wait_for(lambda: '[access] denied (build): privacy %s/kit.json' % kit in self.log_text('player.log'), 60,
                 'the refused build kit')
        retry.touch()
        wait_for(lambda: '[access] still denied (build)' in self.log_text('player.log'), 60, 'still denied')
        self.unlock(kit)
        retry.touch()  # the kit is copied out of the app, then python runs it from there
        wait_for(lambda: 'denied (build): privacy %s' % denied_path in self.log_text('player.log'), 60,
                 'the driver-reported refusal', logs=self.state / 'logs')
        build_log = self.log_text('build.log')
        self.assertIn('build kit copied out of the app', build_log)
        self.assertIn('stand-in driver running from %s' % (self.state / 'Build/kit'), build_log)
        self.assertNotIn(str(kit) + '/game-src', build_log.split('build kit copied')[1])
        retry.touch()  # Try Again runs the driver again (the stand-in now fails plainly)
        wait_for(lambda: 'stand-in driver: second run' in self.log_text('build.log'), 60, 'the second driver run')

    def test_built_game_waits_on_the_play_window(self):
        app = self.make_app(self.case / 'Apps')
        driver = app / 'Contents/Resources/kit/game-src/tools/fable2_game_build.py'
        driver.write_text(r'''import json, os, sys
state = sys.argv[sys.argv.index('--state') + 1]
engine = os.path.join(state, 'Game/Fable II.bundle/Contents/MacOS/fable_2')
os.makedirs(os.path.dirname(engine), exist_ok=True)
open(engine, 'w').write('#!/bin/sh\nexit 0\n')
os.chmod(engine, 0o755)
os.makedirs(os.path.join(state, 'Build'), exist_ok=True)
json.dump(dict(ok=True, kit_id='%s'), open(os.path.join(state, 'Build/build-state.json'), 'w'))
print('[stage] done built')
''' % KIT_ID)
        game = self.state / 'GameData'
        (game / 'data').mkdir(parents=True)
        (game / 'default.xex').write_text('xex')
        (game / '.fable2-portable.json').write_text(json.dumps({'complete': True}))
        tools_ready = self.case / 'clt-installed'
        tools_ready.touch()
        proc = self.launch(app / 'Contents/MacOS/FableIIPlayer', FABLE2_SIMULATE_NO_CLT=str(tools_ready))
        wait_for(lambda: 'the Play window waits for the player' in self.log_text('player.log'), 60, 'the built game',
                 logs=self.state / 'logs')
        self.assertIn('(first build)', self.log_text('player.log'))
        self.assertIsNone(proc.poll())  # the launcher stays open on the Play window
        self.assertNotIn('] play', self.log_text('player.log'))  # no game was started ("play: …" lines)
        self.assertFalse((self.state / 'Build/kit').exists())  # the staged kit is removed after a good build

    def test_move_offer_comes_first(self):
        app = self.make_app(self.downloads)
        game = self.state / 'GameData'
        (game / 'data').mkdir(parents=True)
        (game / 'default.xex').write_text('xex')
        (game / '.fable2-portable.json').write_text(json.dumps({'complete': True}))
        proc = self.launch(app / 'Contents/MacOS/FableIIPlayer', FABLE2_TEST_AUTO_MOVE='1',
                           FABLE2_TEST_MOVE_NO_RELAUNCH='1')
        self.assertEqual(proc.wait(timeout=60), 0)
        self.assertIn('[move] copied to %s' % (self.case / 'Applications/Fable II.app'), self.log_text('player.log'))
        self.assertTrue((self.case / 'Applications/Fable II.app/Contents/Resources/kit/kit.json').is_file())
        self.assertFalse(app.exists())
        # Nothing read the build kit before the offer: no staged kit, no build log.
        self.assertFalse((self.state / 'Build/kit').exists())
        self.assertEqual(self.log_text('build.log'), '')


if __name__ == '__main__':
    unittest.main()
