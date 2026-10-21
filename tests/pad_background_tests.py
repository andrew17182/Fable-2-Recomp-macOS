"""Controllers in the background (FABLE2_PAD_BACKGROUND, src/input/test_background.h).

Compiles test_background.h's ApplyLaunchHints with a stand-in SDL_SetHint and checks which launches get
SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1: normal play by default, never with FABLE2_PAD_BACKGROUND=0, and never in a
test-only instance (FABLE2_PHYSICAL_INPUT=0), which must not read the player's controller. Also checks the pad
driver's active callback in controller_hotkeys.h. Skipped without a C++ compiler or the SDK's SDL3 headers (the build
puts them in out/sdk/mac-arm64/include). Runs under /usr/bin/python3 (3.9)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = next((p for p in (ROOT / 'out/sdk/mac-arm64/include', ROOT / 'out/sdk-src/mac-arm64/include')
                if (p / 'SDL3/SDL_hints.h').is_file()), None)
PROGRAM = r'''
#include "test_background.h"
#include <cstdio>
extern "C" bool SDL_SetHint(const char*, const char*) { return true; }
int main() {
  fable2::testbg::ApplyLaunchHints();
  const char* v = std::getenv(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
  std::printf("%s %d\n", v ? v : "unset", int(fable2::testbg::pad_background()));
}
'''


@unittest.skipUnless(shutil.which('xcrun') and INCLUDE, 'needs a C++ compiler and the SDK SDL3 headers')
class PadBackgroundTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='pad-background-')
        source = Path(cls.temp.name) / 'main.cpp'
        source.write_text(PROGRAM)
        cls.binary = Path(cls.temp.name) / 'pad_background'
        subprocess.run(['xcrun', 'clang++', '-std=c++20', '-I', str(ROOT / 'src/input'), '-I', str(INCLUDE),
                        str(source), '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_with(self, **env):
        clean = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'SDL_'))}
        clean.update(env)
        return subprocess.run([str(self.binary)], env=clean, check=True, capture_output=True,
                              text=True).stdout.split()

    def test_default_on_for_play(self):
        self.assertEqual(self.run_with(), ['1', '1'])

    def test_switch_off(self):
        self.assertEqual(self.run_with(FABLE2_PAD_BACKGROUND='0'), ['unset', '0'])

    def test_never_in_test_only_instances(self):
        self.assertEqual(self.run_with(FABLE2_PHYSICAL_INPUT='0'), ['unset', '0'])
        self.assertEqual(self.run_with(FABLE2_PHYSICAL_INPUT='0', FABLE2_PAD_BACKGROUND='1'), ['unset', '0'])


class PadBackgroundSourceTests(unittest.TestCase):
    def test_pad_driver_stays_active_without_focus_only_when_enabled(self):
        header = (ROOT / 'src/input/controller_hotkeys.h').read_text()
        self.assertIn('if (fable2::testbg::pad_background() && !SDL_GetKeyboardFocus()) return true;\n'
                      '      return is_active();', header)
        self.assertIn('if (!app_active && !fable2::testbg::pad_background())', header)
        self.assertIn('gcshare::MonitorBackgroundEvents(fable2::testbg::pad_background());', header)


if __name__ == '__main__':
    unittest.main()
