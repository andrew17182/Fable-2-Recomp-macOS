"""Settings presets (docs/reference.md "Presets"; tools/macos/Shared/LaunchSettings.swift SettingsPreset, presetRules;
SettingsModel in SettingsViews.swift). A Swift harness built from the shared files checks:
  - Enhanced is the default install; Original is 1x, 30 FPS, intros, no extras, turbo and slow motion off (speed 1.0,
    which the game treats as disabled) with no controller turbo/slow button, the game's stick deadzones;
  - migration: settings from before presets are Enhanced when they match it, else Custom (saved once as the Custom slot);
  - editing a preset setting on Original creates Custom from the Original values plus the edit; the Custom slot survives
    Custom -> Original -> Custom; the last preset persists across a relaunch (a new model on the same store);
  - named presets: save, load, delete, overwrite, built-in names refused, "(edited)" after a change, persistence;
  - --preset for CLI runs (original, enhanced, custom, a saved name);
  - every preset setting is listed in the docs table.
Runs under /usr/bin/python3 (3.9); the Swift part is skipped without Xcode."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
import Foundation
var out: [String: Any] = [:]
let dir = CommandLine.arguments[1]
func env(_ s: LaunchSettings) -> [String] { s.gameEnvironment }
out["defaultPreset"] = LaunchSettings().preset.rawValue
out["defaultMatchesEnhanced"] = LaunchSettings().matches(.enhanced)
let original = LaunchSettings.presetValues(.original)
out["originalEnv"] = env(original)
out["enhancedEnv"] = env(LaunchSettings.presetValues(.enhanced))
out["originalSkipIntro"] = original.skipIntro
out["turboOffLabel"] = [TurboSpeed.label(original.turboSpeed), SlowSpeed.label(original.slowSpeed)]
out["ruleKeys"] = LaunchSettings.presetRules.map(\.key)

// Migration.
let empty = PlistSettingsStore(path: dir + "/empty.plist")
let e = LaunchSettings.load(from: empty)
out["migrateEmpty"] = [e.preset.rawValue, empty.string(forKey: "preset") ?? "-"]
let scaled = PlistSettingsStore(path: dir + "/scaled.plist")
scaled.set("2", forKey: "resolutionScale")
let sc = LaunchSettings.load(from: scaled)
out["migrateScaled"] = [sc.preset.rawValue, scaled.string(forKey: "preset") ?? "-", sc.resolution.rawValue,
                        LaunchSettings.savedCustom(in: scaled)?.resolution.rawValue ?? "-"]
let extras = PlistSettingsStore(path: dir + "/extras.plist")
extras.set(true, forKey: "unlockExtras")
out["migrateExtras"] = LaunchSettings.load(from: extras).preset.rawValue
let fixOnly = PlistSettingsStore(path: dir + "/fix.plist")
fixOnly.set(false, forKey: "turboFineDelay")  // not a preset setting
out["migrateFix"] = LaunchSettings.load(from: fixOnly).preset.rawValue

// Flip to Custom, round trip, persistence.
let path = dir + "/model.plist"
var m = SettingsModel(store: PlistSettingsStore(path: path))
m.choosePreset(.original)
out["afterOriginal"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue, m.settings.frameRate.rawValue]
m.settings.resolution = .double
out["afterEdit"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue, m.settings.frameRate.rawValue,
                    String(m.settings.turboSpeed), m.settings.unlockExtras ? "extras" : "no-extras"]
m.choosePreset(.original)
out["backToOriginal"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue]
m.choosePreset(.enhanced)
out["enhanced"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue, m.settings.frameRate.rawValue]
m.choosePreset(.custom)
out["customAgain"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue, m.settings.frameRate.rawValue]
m.settings.fpsCounter = true  // not a preset setting: stays Custom, no change of slot
m.choosePreset(.original)
out["fpsCounterKept"] = m.settings.fpsCounter
m = SettingsModel(store: PlistSettingsStore(path: path))
out["relaunchOriginal"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue]
m.choosePreset(.custom)
m = SettingsModel(store: PlistSettingsStore(path: path))
out["relaunchCustom"] = [m.settings.preset.rawValue, m.settings.resolution.rawValue, m.settings.frameRate.rawValue]
// Edits on Enhanced replace the Custom slot with Enhanced + the edit.
m.choosePreset(.enhanced)
m.settings.skipIntro = true
m.choosePreset(.original)
m.choosePreset(.custom)
out["slotReplaced"] = [m.settings.resolution.rawValue, m.settings.skipIntro ? "skip" : "intros"]
// Controls edits: turbo button on Original makes Custom.
m.choosePreset(.original)
m.settings.controls.setPad(.rs, for: .turbo)
out["padEdit"] = [m.settings.preset.rawValue, m.settings.padTurbo.rawValue]

// Named presets.
let npath = dir + "/named.plist"
var n = SettingsModel(store: PlistSettingsStore(path: npath))
n.settings.resolution = .triple
out["saveMine"] = n.savePreset(named: " Mine ")
out["saveReserved"] = [n.savePreset(named: "original"), n.savePreset(named: "Enhanced"), n.savePreset(named: "CUSTOM"),
                       n.savePreset(named: "  ")]
out["titleSaved"] = n.presetMenuTitle
n.settings.frameRate = .sixty
out["titleEdited"] = n.presetMenuTitle
n.choosePreset(.enhanced)
out["titleEnhanced"] = n.presetMenuTitle
out["loadMine"] = n.loadPreset(named: "Mine")
out["loaded"] = [n.settings.preset.rawValue, n.settings.resolution.rawValue, n.settings.frameRate.rawValue, n.presetMenuTitle]
n.choosePreset(.enhanced)
n.settings.display = .fullscreen  // not a preset setting
n.choosePreset(.custom)
out["namedBackFromEnhanced"] = n.presetMenuTitle
n.choosePreset(.enhanced)
n.settings.resolution = .double  // a new Custom from Enhanced: no longer "Mine"
out["newCustomFromEnhanced"] = [n.settings.preset.rawValue, n.presetMenuTitle, n.settings.frameRate.rawValue]
_ = n.loadPreset(named: "Mine")
n.settings.turboSpeed = 3.0
_ = n.savePreset(named: "Mine")  // overwrite
_ = n.savePreset(named: "Other")
n = SettingsModel(store: PlistSettingsStore(path: npath))
out["namedRelaunch"] = [n.savedPresetNames.joined(separator: ","), n.presetMenuTitle]
n.choosePreset(.original)
_ = n.loadPreset(named: "Mine")
out["overwritten"] = String(n.settings.turboSpeed)
var cli = LaunchSettings()
out["cliNamed"] = cli.applyCLIPreset("Mine", store: PlistSettingsStore(path: npath))
out["cliNamedValues"] = [cli.preset.rawValue, cli.resolution.rawValue]
out["cliUnknown"] = cli.applyCLIPreset("Nope", store: PlistSettingsStore(path: npath))
// Names: suggestion, rename (built-ins refused, existing names only with replace).
let fresh = SettingsModel(store: PlistSettingsStore(path: dir + "/names.plist"))
out["suggestFresh"] = fresh.suggestedPresetName
_ = fresh.savePreset(named: "Custom 1")
fresh.choosePreset(.enhanced)
out["suggestNext"] = fresh.suggestedPresetName
out["renameOther"] = n.renamePreset("Other", to: "  Third ")
out["renameReserved"] = n.renamePreset("Third", to: "Enhanced")
out["renameTaken"] = n.renamePreset("Third", to: "Mine")
out["renameUnknown"] = n.renamePreset("Nope", to: "X")
out["renamedNames"] = [n.savedPresetNames.joined(separator: ","), n.presetMenuTitle]
out["renameReplace"] = n.renamePreset("Third", to: "Mine", replace: true)
out["afterReplace"] = n.savedPresetNames.joined(separator: ",")
_ = n.savePreset(named: "Other")
n.deletePreset(named: "Other")
n.deletePreset(named: "Mine")
out["afterDelete"] = [n.savedPresetNames.joined(separator: ","), n.presetMenuTitle, n.settings.resolution.rawValue]
n = SettingsModel(store: PlistSettingsStore(path: npath))
out["deletePersisted"] = n.savedPresetNames.count
print(String(decoding: try! JSONSerialization.data(withJSONObject: out, options: [.sortedKeys]), as: UTF8.self))
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class PresetHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='launcher-presets-')
        root = Path(cls.temp.name)
        (root / 'main.swift').write_text(HARNESS)
        binary = root / 'harness'
        subprocess.run(['xcrun', 'swiftc', '-target', 'arm64-apple-macos15.0', '-o', str(binary), str(root / 'main.swift')]
                       + [str(p) for p in sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))],
                       check=True, capture_output=True)
        cls.out = json.loads(subprocess.run([str(binary), str(root)], check=True, capture_output=True,
                                            text=True).stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_enhanced_is_the_default(self):
        self.assertEqual(self.out['defaultPreset'], 'enhanced')
        self.assertTrue(self.out['defaultMatchesEnhanced'])
        e = self.out['enhancedEnv']
        for item in ('FABLE2_FPS_60=auto', 'FABLE2_RES_SCALE=auto', 'FABLE2_SKIP_INTRO_MOVIES=0', 'FABLE2_TURBO_SPEED=2.0',
                     'FABLE2_SLOW_SPEED=0.50', 'FABLE2_UNLOCK_EXTRAS=0', 'FABLE2_FAST_PACING=0'):
            self.assertIn(item, e)

    def test_original_values(self):
        o = self.out['originalEnv']
        for item in ('FABLE2_FPS_60=0', 'REX_VSYNC=1', 'FABLE2_RES_SCALE=1', 'FABLE2_UPSCALING=off',
                     'FABLE2_EFFECTS_NATIVE=1', 'FABLE2_SKIP_INTRO_MOVIES=0', 'FABLE2_TURBO_SPEED=1.0',
                     'FABLE2_SLOW_SPEED=1.00', 'FABLE2_UNLOCK_EXTRAS=0', 'FABLE2_FAST_PACING=0',
                     'FABLE2_CHARACTER_EDITOR=0', 'FABLE2_FIX_LOD_SIZE_CACHE=0', 'FABLE2_PAD_TURBO=none',
                     'FABLE2_PAD_SLOW=none', 'FABLE2_PAD_SCREENSHOT=none'):
            self.assertIn(item, o)
        self.assertFalse([x for x in o if x.startswith('FABLE2_STICK_DEADZONE')])
        # Invisible fixes stay on: no kill switch is passed.
        self.assertFalse([x for x in o if x.endswith('=0') and x.split('=')[0] in (
            'FABLE2_PRESENT_DRAWABLES', 'FABLE2_TURBO_FINE_DELAY', 'FABLE2_RING_REINIT_FIX')])
        self.assertEqual(self.out['turboOffLabel'], ['Off', 'Off'])

    def test_game_treats_speed_one_as_disabled(self):
        turbo = (ROOT / 'src/core/fable2_turbo.h').read_text()
        self.assertIn('if (want != Mode::kOff && ModeSpeed(want) == 1.0)', turbo)
        self.assertIn('if (rounded == 1.0) return 1.0;', turbo)

    def test_migration(self):
        self.assertEqual(self.out['migrateEmpty'], ['enhanced', '-'])
        self.assertEqual(self.out['migrateScaled'], ['custom', 'custom', '2', '2'])
        self.assertEqual(self.out['migrateExtras'], 'custom')
        self.assertEqual(self.out['migrateFix'], 'enhanced')

    def test_edit_flips_to_custom_from_the_preset_base(self):
        self.assertEqual(self.out['afterOriginal'], ['original', '1', '30'])
        self.assertEqual(self.out['afterEdit'], ['custom', '2', '30', '1.0', 'no-extras'])
        self.assertEqual(self.out['padEdit'], ['custom', 'rs'])

    def test_custom_round_trip_and_relaunch(self):
        self.assertEqual(self.out['backToOriginal'], ['original', '1'])
        self.assertEqual(self.out['enhanced'], ['enhanced', 'auto', 'auto'])
        self.assertEqual(self.out['customAgain'], ['custom', '2', '30'])
        self.assertTrue(self.out['fpsCounterKept'])
        self.assertEqual(self.out['relaunchOriginal'], ['original', '1'])
        self.assertEqual(self.out['relaunchCustom'], ['custom', '2', '30'])
        self.assertEqual(self.out['slotReplaced'], ['auto', 'skip'])

    def test_named_presets(self):
        self.assertTrue(self.out['saveMine'])
        self.assertEqual(self.out['saveReserved'], [False, False, False, False])
        self.assertEqual(self.out['titleSaved'], 'Mine')
        self.assertEqual(self.out['titleEdited'], 'Mine (edited)')
        self.assertEqual(self.out['titleEnhanced'], 'Presets')
        self.assertTrue(self.out['loadMine'])
        self.assertEqual(self.out['loaded'], ['custom', '3', 'auto', 'Mine'])
        self.assertEqual(self.out['namedBackFromEnhanced'], 'Mine')
        self.assertEqual(self.out['newCustomFromEnhanced'], ['custom', 'Presets', 'auto'])
        self.assertEqual(self.out['namedRelaunch'], ['Mine,Other', 'Other'])
        self.assertEqual(self.out['overwritten'], '3.0')
        self.assertEqual(self.out['suggestFresh'], 'Custom 1')
        self.assertEqual(self.out['suggestNext'], 'Custom 2')
        self.assertTrue(self.out['renameOther'])
        self.assertEqual([self.out['renameReserved'], self.out['renameTaken'], self.out['renameUnknown']],
                         [False, False, False])
        self.assertEqual(self.out['renamedNames'], ['Mine,Third', 'Mine'])
        self.assertTrue(self.out['renameReplace'])
        self.assertEqual(self.out['afterReplace'], 'Mine')
        self.assertTrue(self.out['cliNamed'])
        self.assertEqual(self.out['cliNamedValues'], ['custom', '3'])
        self.assertFalse(self.out['cliUnknown'])
        self.assertEqual(self.out['afterDelete'], ['', 'Presets', '3'])
        self.assertEqual(self.out['deletePersisted'], 0)

    def test_docs_table_lists_every_preset_setting(self):
        doc = (ROOT / 'docs/reference.md').read_text()
        section = doc[doc.index('### Presets'):]
        section = section[:section.index('\n### ', 5)]
        for key in self.out['ruleKeys']:
            self.assertIn('`%s`' % key, section)


class PresetSourceTests(unittest.TestCase):
    def test_picker_on_both_front_pages(self):
        dev = (ROOT / 'tools/macos/DevLauncher/DevLauncher.swift').read_text()
        player = (ROOT / 'tools/macos/Player/main.swift').read_text()
        for text in (dev, player):
            self.assertIn('Section { PresetPicker(model: model) }', text)
            self.assertLess(text.index('PresetPicker(model: model)'), text.index('SaveMenu(choices:'))
        self.assertIn('"--preset"', player[:player.index('var gameArguments')])
        self.assertIn('case "--preset":', dev)

    def test_fixed_widths(self):
        pages = (ROOT / 'tools/macos/Shared/LauncherPages.swift').read_text()
        picker = pages[pages.index('struct PresetPicker'):pages.index('struct SaveMenu')]
        self.assertIn('.frame(width: Self.width)', picker)
        self.assertIn('.frame(width: Self.menuWidth', picker)
        self.assertTrue(re.search(r'\.pickerStyle\(\.segmented\)', picker))


if __name__ == '__main__':
    unittest.main()
