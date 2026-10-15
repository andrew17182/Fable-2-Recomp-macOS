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
        self.assertEqual(len(inputs), 20)
        for name in inputs: self.assertIn(name.upper(), accepted)
        aliases = dict(re.findall(r'"([A-Z0-9]+)": "([A-Za-z0-9]+)"',
                                  SWIFT[SWIFT.index('static let aliases'):SWIFT.index('static let builtInDefault')]))
        for alias, target in aliases.items():
            self.assertIn(alias, accepted)
            self.assertIn(target, inputs)

    def test_default_layout_matches_game(self):
        swift = ''.join(re.findall(r'"([^"]*)"', SWIFT[SWIFT.index('static let builtInDefault'):
                                                         SWIFT.index('static func canonicalTarget')]))
        config_h = (ROOT / 'src/core/fable2_config.h').read_text()
        cpp = ''.join(re.findall(r'"([^"]*)"', config_h[config_h.index('kDefaultKeyboardGamepadMap ='):
                                                       config_h.index(';', config_h.index('kDefaultKeyboardGamepadMap ='))]))
        toml = re.search(r'^keyboard_gamepad_map = "([^"]*)"', (ROOT / 'config/fable2_config.toml').read_text(), re.M)
        self.assertEqual(swift, cpp)
        self.assertEqual(swift, toml.group(1))

    def test_hotkey_defaults_match_code(self):
        rebindable = dict(re.findall(r'keys: "([^"]+)",\s*detail: "[^"]*",\s*cvar: "(\w+)"', SWIFT))
        rebindable = {cvar: key for key, cvar in rebindable.items()}
        hotkeys = (ROOT / 'src/input/recomp_hotkeys.h').read_text()
        for cvar, key in re.findall(r'Binding\{"(\w+)", "([^"]+)"\}', hotkeys):
            self.assertEqual(rebindable.pop(cvar), key)
        self.assertEqual(rebindable, {'bind_console': 'Backtick'})  # the SDK default, kept by the recomp
        keys = (ROOT / 'src/input/window_keyboard_mouse.h').read_text()
        for key in ('k5', 'kF5', 'k6', 'kF6', 'k7', 'kF7', 'kF9'): self.assertIn('K::%s)' % key, keys)

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
out["envDefault"] = LaunchSettings().settingsEnvironment.filter { $0.hasPrefix("REX_") || $0.hasPrefix("FABLE2_VIEW") }
var c = ControlSettings()
var results: [String] = []
for (arg, value) in [("--keyboard-map", "Return:Pause,e:A"), ("--keyboard-map", "Return:pause"), ("--mouse-look", ""),
                     ("--mouse-look-scale", "5000"), ("--mouse-look-scale", "700"), ("--bind", "bind_settings=F4"),
                     ("--bind", "bind_console=Nope"), ("--view-button-to-game", ""), ("--other", "")] {
  switch c.applyCLI(arg, value) {
  case .notMine: results.append("notMine")
  case .ok(let n): results.append("ok\(n)")
  case .error: results.append("error")
  }
}
out["cli"] = results
out["envCLI"] = c.environment
let store = PlistSettingsStore(path: CommandLine.arguments[1])
var s = LaunchSettings()
let old = s
s.controls = c
s.save(to: store, old: old)
let reloaded = LaunchSettings.load(from: PlistSettingsStore(path: CommandLine.arguments[1]))
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
        self.assertEqual(self.out['defaultMap'], 'E:A,X:B,Z:X,C:Y,Q:LT,Tab:RT,Up:Up,Down:Down,Left:Left,Right:Right,'
                                                 'Escape:Pause,M:Select,W:StickUp,S:StickDown,A:StickLeft,D:StickRight')

    def test_conflicts(self):
        # Replace: X leaves B (B unbound). Swap: B gets A's old key E. Add: A keeps E. Add never swaps.
        self.assertTrue(self.out['replace'].startswith('X:A,Z:X,'))
        self.assertTrue(self.out['swap'].startswith('X:A,E:B,Z:X,'))
        self.assertTrue(self.out['add'].startswith('E:A,Space:A,X:B,'))
        self.assertTrue(self.out['move'].startswith('E:A,X:A,Z:X,'))

    def test_environment(self):
        self.assertEqual(self.out['envDefault'], [
            'REX_KEYBOARD_GAMEPAD_MAP=' + self.out['defaultMap'], 'REX_MOUSE_LOOK=false', 'REX_MOUSE_LOOK_SCALE=256'])
        self.assertEqual(self.out['cli'], ['error', 'ok1', 'ok0', 'error', 'ok1', 'ok1', 'error', 'ok0', 'notMine'])
        self.assertEqual(self.out['envCLI'], [
            'REX_KEYBOARD_GAMEPAD_MAP=Return:pause', 'REX_MOUSE_LOOK=true', 'REX_MOUSE_LOOK_SCALE=700',
            'REX_BIND_SETTINGS=F4', 'REX_MOUSE_UNLOCK_KEY=F4', 'FABLE2_VIEW_BUTTON_TO_GAME=1'])

    def test_survives_reload(self):
        self.assertTrue(self.out['persisted'])


if __name__ == '__main__':
    unittest.main()
