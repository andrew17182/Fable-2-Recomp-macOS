"""Settings > Controls (tools/macos/Shared/ControlsView.swift), shared by the Dev Launcher and the player app.

- Key names: the Swift list must be exactly the names rex::ui::ParseVirtualKey accepts (kKeyNames in the SDK's
  src/ui/keybinds.cpp; exact case), so every key the tab writes round-trips into the game. The macOS keycode
  table may only produce names from that list.
- Guest inputs and aliases must be names FillGuestTarget (src/input/keyboard_gamepad.h) accepts, and the
  built-in layout must equal fable2::config::kDefaultKeyboardGamepadMap and config/fable2_config.toml.
- Hotkey defaults must match src/input/recomp_hotkeys.h / the SDK, and the fixed keys the code handles.
- A Swift harness (built with swiftc from the shared files) checks parse/serialize, conflict replace/swap/add,
  the environment items, the --launch options, and that the settings survive a reload of the store.

Runs under /usr/bin/python3 (3.9). The Swift part is skipped without Xcode; the SDK comparison is skipped when
no SDK source is present (the build extracts it to out/sdk-src/source)."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SWIFT = (ROOT / 'tools/macos/Shared/ControlsView.swift').read_text()
SDK_CANDIDATES = [ROOT / 'out/sdk-src/source', ROOT / 'thirdparty/rexglue-sdk',
                  ROOT.parent / 'Fable-2-Recomp-macOS/out/sdk-src/source',
                  ROOT.parents[1] / 'Fable-2-Recomp-macOS/out/sdk-src/source']


def block(name):
    m = re.search(r'// BEGIN %s.*?\n(.*?)// END %s' % (re.escape(name), re.escape(name)), SWIFT, re.S)
    assert m, name
    return m.group(1)


def swift_strings(text):
    return re.findall(r'"([^"]*)"', text)


def sdk_key_names():
    for base in SDK_CANDIDATES:
        path = base / 'src/ui/keybinds.cpp'
        if path.is_file():
            text = path.read_text()
            table = text[text.index('kKeyNames'):text.index('};', text.index('kKeyNames'))]
            return re.findall(r'\{"([^"]+)",\s*VirtualKey::', table)
    return None


class ControlsSourceTests(unittest.TestCase):
    def test_key_names_match_parse_virtual_key(self):
        sdk = sdk_key_names()
        if sdk is None: self.skipTest('no SDK source (build once to extract out/sdk-src/source)')
        self.assertEqual(swift_strings(block('sdk-key-names')), sdk)

    def test_keycodes_produce_valid_names(self):
        names = set(swift_strings(block('sdk-key-names')))
        produced = set(re.findall(r'\d+: "([^"]+)"', block('mac-keycodes')))
        self.assertTrue(produced)
        self.assertLessEqual(produced, names)

    def test_guest_inputs_are_game_targets(self):
        header = (ROOT / 'src/input/keyboard_gamepad.h').read_text()
        accepted = set(re.findall(r'u == "([A-Z0-9]+)"', header))
        inputs = re.findall(r'GuestInput\(name: "([^"]+)"', block('guest-inputs'))
        self.assertEqual(len(inputs), 24)
        for name in inputs: self.assertIn(name.upper(), accepted)
        aliases = dict(re.findall(r'"([A-Z0-9]+)": "([A-Za-z0-9]+)"',
                                  SWIFT[SWIFT.index('static let aliases'):SWIFT.index('static let builtInDefault')]))
        for alias, target in aliases.items():
            self.assertIn(alias, accepted)
            self.assertIn(target, inputs)

    def test_default_layout_matches_game(self):
        swift = ''.join(re.findall(r'"([^"]*)"', SWIFT[SWIFT.index('static let builtInDefault'):
                                                         SWIFT.index('/// Map format version')]))
        config_h = (ROOT / 'src/core/fable2_config.h').read_text()
        cpp = ''.join(re.findall(r'"([^"]*)"', config_h[config_h.index('kDefaultKeyboardGamepadMap ='):
                                                       config_h.index(';', config_h.index('kDefaultKeyboardGamepadMap ='))]))
        toml = re.search(r'^keyboard_gamepad_map = "([^"]*)"', (ROOT / 'config/fable2_config.toml').read_text(), re.M)
        self.assertEqual(swift, cpp)
        self.assertEqual(swift, toml.group(1))

    def test_default_keys_are_parse_virtual_key_names(self):
        # Every key in the built-in layout must be a name ParseVirtualKey accepts (else the game skips it), and
        # none may be a fixed or default overlay hotkey.
        swift = ''.join(re.findall(r'"([^"]*)"', SWIFT[SWIFT.index('static let builtInDefault'):
                                                         SWIFT.index('/// Map format version')]))
        keys = [entry.split(':')[0] for entry in swift.split(',')]
        names = set(swift_strings(block('sdk-key-names')))
        sdk = sdk_key_names()
        hotkeys = set(re.findall(r'Hotkey\(title: "[^"]+", keys: "([^"]+)"', SWIFT)) | {'F5', 'F6', 'F7', 'F8', 'F9', '5', '6', '7'}
        for key in keys:
            self.assertIn(key, names)
            if sdk is not None: self.assertIn(key, sdk)
            self.assertNotIn(key, hotkeys)
        for key in ('I', 'J', 'K', 'L', 'Up', 'Down', 'Left', 'Right'): self.assertIn(key, keys)

    def test_hotkey_defaults_match_code(self):
        rebindable = dict(re.findall(r'keys: "([^"]+)",\s*detail: "[^"]*",\s*cvar: "(\w+)"', SWIFT))
        rebindable = {cvar: key for key, cvar in rebindable.items()}
        hotkeys = (ROOT / 'src/input/recomp_hotkeys.h').read_text()
        for cvar, key in re.findall(r'Binding\{"(\w+)", "([^"]+)"\}', hotkeys):
            self.assertEqual(rebindable.pop(cvar), key)
        self.assertEqual(rebindable, {'bind_console': 'Backtick'})  # the SDK default, kept by the recomp
        keys = (ROOT / 'src/input/window_keyboard_mouse.h').read_text()
        for key in ('k5', 'kF5', 'k6', 'kF6', 'k7', 'kF7', 'kF8', 'kF9'): self.assertIn('K::%s)' % key, keys)

    def test_registered_in_both_apps(self):
        self.assertIn('case general, display, controls,', (ROOT / 'tools/macos/Shared/LaunchSettings.swift').read_text())
        self.assertIn('case .controls:', (ROOT / 'tools/macos/Shared/SettingsViews.swift').read_text())
        self.assertIn('tools/macos/Shared/*.swift', (ROOT / 'tools/macos/build_dev_launcher.sh').read_text())
        import sys
        sys.path.insert(0, str(ROOT / 'tools'))
        import fable2_portable
        self.assertIn('tools/macos/Shared/ControlsView.swift', fable2_portable.PLAYER_SOURCES)


HARNESS = r'''
import Foundation
var out: [String: Any] = [:]
let base = KeyboardMap.parse(KeyboardMap.defaultMap).bindings
let p = KeyboardMap.parse("E:a,Space:start,Bogus:A,E:Nope,X,W:StickUp,W:StickUp,LMB:rthumb")
out["parse"] = ["bindings": p.bindings, "invalid": p.invalid]
out["defaultMap"] = KeyboardMap.defaultMap
out["replace"] = KeyboardMap.serialize(KeyboardMap.bind("X", to: "A", add: false, swap: false, in: base))
out["swap"] = KeyboardMap.serialize(KeyboardMap.bind("X", to: "A", add: false, swap: true, in: base))
out["add"] = KeyboardMap.serialize(KeyboardMap.bind("Space", to: "A", add: true, swap: false, in: base))
out["move"] = KeyboardMap.serialize(KeyboardMap.bind("X", to: "A", add: true, swap: true, in: base))
let oldDefault = "E:A,X:B,Z:X,C:Y,W:StickUp,S:StickDown,A:StickLeft,D:StickRight,Escape:Pause,M:Select,Q:LT,Tab:RT,"
  + "Up:Up,Down:Down,Left:Left,Right:Right"
out["migrateOld"] = KeyboardMap.migrated(oldDefault)
out["migrateOldTable"] = KeyboardMap.migrated(KeyboardMap.serialize(KeyboardMap.parse(oldDefault).bindings))
out["migrateCustom"] = KeyboardMap.migrated("Space:A,I:B,Up:Up,Down:Down,Left:Left,Right:Right")
out["migrateOther"] = KeyboardMap.migrated("E:A,Up:Up,Down:Down,Left:Left,Right:Y")
out["migrateNew"] = KeyboardMap.migrated(KeyboardMap.defaultMap)
let mstore = PlistSettingsStore(path: CommandLine.arguments[1] + ".migrate")
mstore.set(oldDefault, forKey: "keyboardMap")
out["loadOld"] = ControlSettings.load(from: mstore).keyboardMap
out["loadOldStored"] = mstore.string(forKey: "keyboardMap") ?? ""
mstore.set("Up:Up,Down:Down,Left:Left,Right:Right", forKey: "keyboardMap")  // chosen after migrating: kept
out["loadChosen"] = ControlSettings.load(from: mstore).keyboardMap
let fresh = PlistSettingsStore(path: CommandLine.arguments[1] + ".fresh")
out["loadFresh"] = ControlSettings.load(from: fresh).keyboardMap
out["envDefault"] = LaunchSettings().settingsEnvironment.filter { $0.hasPrefix("REX_") || $0.hasPrefix("FABLE2_VIEW") }
var c = ControlSettings()
var results: [String] = []
for (arg, value) in [("--keyboard-map", "Return:Pause,e:A"), ("--keyboard-map", "Return:pause"), ("--mouse-look", ""),
                     ("--mouse-look-scale", "5000"), ("--mouse-look-scale", "700"), ("--bind", "bind_settings=F4"),
                     ("--bind", "bind_console=Nope"), ("--view-button-to-game", ""), ("--pad-screenshot", "view"),
                     ("--pad-quicksave", "bogus"), ("--other", "")] {
  switch c.applyCLI(arg, value) {
  case .notMine: results.append("notMine")
  case .ok(let n): results.append("ok\(n)")
  case .error: results.append("error")
  }
}
out["cli"] = results
var sticks = ControlSettings()
var stickResults: [String] = []
for (arg, value) in [("--stick-deadzone-left", "5"), ("--stick-deadzone-right", "51"), ("--stick-deadzone-right", "x"),
                     ("--stick-deadzone-right", "0"), ("--no-stick-anti-deadzone", "")] {
  switch sticks.applyCLI(arg, value) {
  case .notMine: stickResults.append("notMine")
  case .ok(let n): stickResults.append("ok\(n)")
  case .error: stickResults.append("error")
  }
}
out["stickCLI"] = stickResults
out["stickEnv"] = sticks.environment.filter { $0.hasPrefix("FABLE2_STICK_") }
out["stickDefaultEnv"] = ControlSettings().environment.filter { $0.hasPrefix("FABLE2_STICK_") }
_ = sticks.applyCLI("--stick-deadzone-left", "game")
out["stickGameEnv"] = sticks.environment.filter { $0.hasPrefix("FABLE2_STICK_") }
let stickStore = PlistSettingsStore(path: CommandLine.arguments[1] + ".sticks")
var stickSaved = ControlSettings()
stickSaved.stickDeadzoneRight = 12
stickSaved.save(to: stickStore, old: ControlSettings())
let stickLoaded = ControlSettings.load(from: stickStore)
out["stickPersist"] = [stickLoaded.stickDeadzoneLeft.map(String.init) ?? "game", stickLoaded.stickDeadzoneRight.map(String.init) ?? "game"]
var stickReset = stickLoaded
stickReset.stickDeadzoneRight = nil
stickReset.save(to: stickStore, old: stickLoaded)
out["stickReset"] = ControlSettings.load(from: stickStore).stickDeadzoneRight.map(String.init) ?? "game"
// The old "Skip game deadzone" toggle (until 2026-10-22): a stored choice is dropped and never sent.
stickStore.set(false, forKey: "stickAntiDeadzone")
let stickStale = ControlSettings.load(from: stickStore)
out["stickStaleKey"] = stickStore.object(forKey: "stickAntiDeadzone") == nil ? "removed" : "kept"
out["stickStaleEnv"] = stickStale.environment.filter { $0.hasPrefix("FABLE2_STICK_ANTI") }
// Walk deadzone (FABLE2_WALK_DEADZONE, 0...25; game/original = off).
var walk = ControlSettings()
var walkResults: [String] = []
for (arg, value) in [("--walk-deadzone", "5"), ("--walk-deadzone", "26"), ("--walk-deadzone", "x"),
                     ("--walk-deadzone", "original"), ("--walk-deadzone", "0"), ("--stick-deadzone-left", "original")] {
  switch walk.applyCLI(arg, value) {
  case .notMine: walkResults.append("notMine")
  case .ok(let n): walkResults.append("ok\(n)")
  case .error: walkResults.append("error")
  }
}
out["walkCLI"] = walkResults
out["walkEnv"] = walk.environment.filter { $0.hasPrefix("FABLE2_WALK_") || $0.hasPrefix("FABLE2_STICK_") }
out["walkDefaultEnv"] = ControlSettings().environment.filter { $0.hasPrefix("FABLE2_WALK_") }
var walkSaved = ControlSettings()
walkSaved.walkDeadzone = 7
walkSaved.save(to: stickStore, old: ControlSettings())
out["walkPersist"] = ControlSettings.load(from: stickStore).walkDeadzone.map(String.init) ?? "game"
var pads = ControlSettings()
out["padDefault"] = pads.environment.filter { $0.hasPrefix("FABLE2_PAD_") }
pads.setPad(.ls, for: .turbo)
out["padLs"] = pads.environment.filter { $0.hasPrefix("FABLE2_PAD_") }
pads.setPad(.share, for: .turbo)  // Share moves from quick save to turbo
out["padMove"] = pads.environment.filter { $0.hasPrefix("FABLE2_PAD_") }
pads.setPad(.share, for: .quickSave)
pads.setPad(.none, for: .turbo)
out["padBack"] = pads.environment.filter { $0.hasPrefix("FABLE2_PAD_") } + [pads.pad.isEmpty ? "empty" : "set"]
func padLoad(_ name: String, _ stored: [String: Any]) -> [String] {
  let st = PlistSettingsStore(path: CommandLine.arguments[1] + "." + name)
  for (k, v) in stored { st.set(v, forKey: k) }
  let loaded = ControlSettings.load(from: st)
  return PadAction.allCases.map { "\($0.rawValue)=\(loaded.padButton($0).rawValue)" }
    + [st.string(forKey: "padTurbo") ?? "-", st.string(forKey: "padScreenshot") ?? "-",
       String(st.object(forKey: "padDefaultsVersion") as? Int ?? 0)]
}
out["padLoadFresh"] = padLoad("pfresh", [:])
// Restore Defaults used to store the old defaults: treated as unset.
out["padLoadOldDefaults"] = padLoad("pold", ["padTurbo": "none", "padScreenshot": "view"])
// A real choice from before is kept; turbo's new default stays off View while the screenshot has it.
out["padLoadOldCustom"] = padLoad("pcustom", ["padScreenshot": "view", "padQuickSave": "none"])
out["padLoadOldCustomFree"] = padLoad("pfree", ["padScreenshot": "ls"])
// After the migration, an explicit screenshot=View (turbo moved to None) is kept.
out["padLoadNew"] = padLoad("pnew", ["padScreenshot": "view", "padTurbo": "none", "padDefaultsVersion": 2])
var padSaved = ControlSettings()
let padStore = PlistSettingsStore(path: CommandLine.arguments[1] + ".psave")
var padOld = padSaved
padSaved.setPad(.view, for: .screenshot)
padSaved.save(to: padStore, old: padOld)
padOld = padSaved
padSaved.pad = [:]  // Restore Defaults
padSaved.save(to: padStore, old: padOld)
out["padSaveDefaults"] = [padStore.string(forKey: "padTurbo") ?? "-", padStore.string(forKey: "padScreenshot") ?? "-"]
out["envCLI"] = c.environment
let store = PlistSettingsStore(path: CommandLine.arguments[1])
var s = LaunchSettings()
let old = s
s.controls = c
s.save(to: store, old: old)
var reloaded = LaunchSettings.load(from: PlistSettingsStore(path: CommandLine.arguments[1]))
reloaded.preset = s.preset  // the moved turbo button makes it Custom on load (tests/launcher_presets_tests.py)
out["persisted"] = reloaded.controls == c && reloaded == s
print(String(decoding: try! JSONSerialization.data(withJSONObject: out, options: [.sortedKeys]), as: UTF8.self))
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class ControlsHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='controls-tab-')
        root = Path(cls.temp.name)
        (root / 'main.swift').write_text(HARNESS)
        binary = root / 'harness'
        subprocess.run(['xcrun', 'swiftc', '-target', 'arm64-apple-macos14.0', '-o', str(binary), str(root / 'main.swift')]
                       + [str(p) for p in sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))],
                       check=True, capture_output=True)
        out = subprocess.run([str(binary), str(root / 'store.plist')], check=True, capture_output=True, text=True).stdout
        cls.out = json.loads(out)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_parse(self):
        parse = self.out['parse']
        self.assertEqual(parse['bindings'], {'A': ['E'], 'Pause': ['Space'], 'StickUp': ['W'], 'R3': ['LMB']})
        self.assertEqual(parse['invalid'], ['Bogus:A', 'E:Nope', 'X'])

    def test_default_in_table_order(self):
        self.assertEqual(self.out['defaultMap'], 'E:A,X:B,Z:X,C:Y,Q:LT,Tab:RT,I:Up,K:Down,J:Left,L:Right,'
                                                 'Escape:Pause,M:Select,W:StickUp,S:StickDown,A:StickLeft,D:StickRight,'
                                                 'Up:RStickUp,Down:RStickDown,Left:RStickLeft,Right:RStickRight')

    def test_migration(self):
        new = self.out['defaultMap']
        self.assertEqual(self.out['migrateOld'], new)
        self.assertEqual(self.out['migrateOldTable'], new)
        self.assertEqual(self.out['migrateNew'], new)
        # Custom map with the old arrows: arrows -> right stick, I/J/K/L -> d-pad (I leaves B), rest kept.
        self.assertEqual(self.out['migrateCustom'], 'Space:A,I:Up,K:Down,J:Left,L:Right,'
                                                    'Up:RStickUp,Down:RStickDown,Left:RStickLeft,Right:RStickRight')
        self.assertEqual(self.out['migrateOther'], 'E:A,Up:Up,Down:Down,Left:Left,Right:Y')
        self.assertEqual(self.out['loadOld'], new)
        self.assertEqual(self.out['loadOldStored'], new)
        self.assertEqual(self.out['loadChosen'], 'Up:Up,Down:Down,Left:Left,Right:Right')
        self.assertEqual(self.out['loadFresh'], new)

    def test_conflicts(self):
        # Replace: X leaves B (B unbound). Swap: B gets A's old key E. Add: A keeps E. Add never swaps.
        self.assertTrue(self.out['replace'].startswith('X:A,Z:X,'))
        self.assertTrue(self.out['swap'].startswith('X:A,E:B,Z:X,'))
        self.assertTrue(self.out['add'].startswith('E:A,Space:A,X:B,'))
        self.assertTrue(self.out['move'].startswith('E:A,X:A,Z:X,'))

    def test_environment(self):
        self.assertEqual(self.out['envDefault'], [
            'REX_KEYBOARD_GAMEPAD_MAP=' + self.out['defaultMap'], 'REX_MOUSE_LOOK=false', 'REX_MOUSE_LOOK_SCALE=256'])
        self.assertEqual(self.out['cli'], ['error', 'ok1', 'ok0', 'error', 'ok1', 'ok1', 'error', 'ok0', 'ok1', 'error',
                                           'notMine'])
        # --pad-screenshot view takes View from turbo.
        self.assertEqual(self.out['envCLI'], [
            'REX_KEYBOARD_GAMEPAD_MAP=Return:pause', 'REX_MOUSE_LOOK=true', 'REX_MOUSE_LOOK_SCALE=700',
            'REX_BIND_SETTINGS=F4', 'REX_MOUSE_UNLOCK_KEY=F4', 'FABLE2_VIEW_BUTTON_TO_GAME=1',
            'FABLE2_PAD_TURBO=none', 'FABLE2_PAD_SLOW=none', 'FABLE2_PAD_QUICKSAVE=share', 'FABLE2_PAD_SCREENSHOT=view', 'FABLE2_PAD_FRAMEGEN=none'])

    def test_stick_deadzones(self):
        # --no-stick-anti-deadzone is gone (2026-10-22): the game's deadzone is always skipped with a value set.
        self.assertEqual(self.out['stickCLI'], ['ok1', 'error', 'error', 'ok1', 'notMine'])
        self.assertEqual(self.out['stickEnv'], ['FABLE2_STICK_DEADZONE_LEFT=5', 'FABLE2_STICK_DEADZONE_RIGHT=0'])
        self.assertEqual(self.out['stickDefaultEnv'], [])  # game default: nothing passed
        self.assertEqual(self.out['stickGameEnv'], ['FABLE2_STICK_DEADZONE_RIGHT=0'])
        self.assertEqual(self.out['stickStaleKey'], 'removed')
        self.assertEqual(self.out['stickStaleEnv'], [])
        swift = (ROOT / 'tools/macos/Shared/ControlsView.swift').read_text()
        self.assertNotIn('Skip game deadzone', swift)
        self.assertEqual(self.out['stickPersist'], ['game', '12'])
        self.assertEqual(self.out['stickReset'], 'game')
        header = (ROOT / 'src/input/stick_deadzone.h').read_text()
        for name in ('FABLE2_STICK_DEADZONE_LEFT', 'FABLE2_STICK_DEADZONE_RIGHT', 'FABLE2_STICK_ANTI_DEADZONE',
                     'FABLE2_WALK_DEADZONE'):
            self.assertIn('"%s"' % name, header)
        # The sliders' off position reads "Original" (2026-10-22; was "Game"); `game` stays the CLI/env value.
        self.assertIn('?? "Original"', swift)
        self.assertNotIn('?? "Game"', swift)

    def test_walk_deadzone(self):
        self.assertEqual(self.out['walkCLI'], ['ok1', 'error', 'error', 'ok1', 'ok1', 'ok1'])
        self.assertEqual(self.out['walkEnv'], ['FABLE2_WALK_DEADZONE=0'])
        self.assertEqual(self.out['walkDefaultEnv'], [])
        self.assertEqual(self.out['walkPersist'], '7')
        swift = (ROOT / 'tools/macos/Shared/ControlsView.swift').read_text()
        self.assertIn('deadzoneRow("Walk deadzone"', swift)

    def test_controller_hotkeys(self):
        self.assertEqual(self.out['padDefault'], [])
        self.assertEqual(self.out['padLs'], ['FABLE2_PAD_TURBO=ls', 'FABLE2_PAD_SLOW=none', 'FABLE2_PAD_QUICKSAVE=share',
                                             'FABLE2_PAD_SCREENSHOT=none', 'FABLE2_PAD_FRAMEGEN=none'])
        self.assertEqual(self.out['padMove'], ['FABLE2_PAD_TURBO=share', 'FABLE2_PAD_SLOW=none', 'FABLE2_PAD_QUICKSAVE=none',
                                               'FABLE2_PAD_SCREENSHOT=none', 'FABLE2_PAD_FRAMEGEN=none'])
        self.assertEqual(self.out['padBack'], ['FABLE2_PAD_TURBO=none', 'FABLE2_PAD_SLOW=none',
                                               'FABLE2_PAD_QUICKSAVE=share', 'FABLE2_PAD_SCREENSHOT=none', 'FABLE2_PAD_FRAMEGEN=none', 'set'])

    def test_controller_hotkey_defaults_load(self):
        new = ['turbo=view', 'slow=none', 'quickSave=share', 'screenshot=none', 'frameGen=none']
        self.assertEqual(self.out['padLoadFresh'], new + ['-', '-', '2'])
        self.assertEqual(self.out['padLoadOldDefaults'], new + ['-', '-', '2'])
        self.assertEqual(self.out['padLoadOldCustom'],
                         ['turbo=none', 'slow=none', 'quickSave=none', 'screenshot=view', 'frameGen=none', '-', 'view', '2'])
        self.assertEqual(self.out['padLoadOldCustomFree'],
                         ['turbo=view', 'slow=none', 'quickSave=share', 'screenshot=ls', 'frameGen=none', '-', 'ls', '2'])
        self.assertEqual(self.out['padLoadNew'],
                         ['turbo=none', 'slow=none', 'quickSave=share', 'screenshot=view', 'frameGen=none', 'none', 'view', '2'])
        # Defaults are stored as no entry.
        self.assertEqual(self.out['padSaveDefaults'], ['-', '-'])

    def test_game_and_launcher_defaults_agree(self):
        header = (ROOT / 'src/input/controller_hotkeys.h').read_text()
        game = dict(re.findall(r'\{Action::k\w+, "(FABLE2_PAD_\w+)", (\w+|-1)\}', header))
        names = {'kView': 'view', 'kShare': 'share', 'kLeftStick': 'ls', 'kRightStick': 'rs', '-1': 'none'}
        game = {k: names[v] for k, v in game.items()}
        self.assertEqual(game, {'FABLE2_PAD_TURBO': 'view', 'FABLE2_PAD_SLOW': 'none',
                                'FABLE2_PAD_QUICKSAVE': 'share', 'FABLE2_PAD_SCREENSHOT': 'none',
                                'FABLE2_PAD_FRAMEGEN': 'none'})
        self.assertEqual(self.out['padLoadFresh'][:5], ['turbo=view', 'slow=none', 'quickSave=share', 'screenshot=none', 'frameGen=none'])

    def test_game_reads_launcher_names(self):
        header = (ROOT / 'src/input/controller_hotkeys.h').read_text()
        for name in ('FABLE2_PAD_TURBO', 'FABLE2_PAD_SLOW', 'FABLE2_PAD_QUICKSAVE', 'FABLE2_PAD_SCREENSHOT',
                     'FABLE2_PAD_FRAMEGEN'):
            self.assertIn('"%s"' % name, header)
        for value in ('none', 'view', 'share', 'ls', 'rs'):
            self.assertIn('"%s"' % value, header)

    def test_survives_reload(self):
        self.assertTrue(self.out['persisted'])


if __name__ == '__main__':
    unittest.main()
