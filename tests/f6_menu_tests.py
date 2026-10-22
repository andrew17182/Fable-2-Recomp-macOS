"""F6 menu: fast page switches and a close path (src/lua/F6.lua, src/core/fable2_f6_close.h).

Checks the B tap that dismisses the open game menu box (compiled with the system C++ compiler: press edge
on the next read, release after kTapPolls reads, packet number raised at each change), and the menu
scripts' structure: every box goes through F6.lua's shared st.box (no fixed 30-tick waits), paged menus
keep their page-flip entry in slot 1, sub-scripts use the F6 state (not the old F5 one) and unwind on
nil (F6 closed the menu), the close ack file exists and matches the host's name, and F6 is not gated by
the ImGui overlay. Runs under /usr/bin/python3 (3.9)."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / 'src/lua'
SUBSCRIPTS = ['character_editor.lua', 'modifyMoney.lua', 'modifyMoralityPurity.lua']

PROGRAM = r'''
#include "fable2_f6_close.h"
#include <cstdio>
static void read(uint8_t* s, uint32_t packet, uint16_t buttons) {
  s[0] = packet >> 24; s[1] = packet >> 16; s[2] = packet >> 8; s[3] = packet;
  s[4] = buttons >> 8; s[5] = buttons;
  fable2::f6close::InjectTap(s);
  unsigned p = (unsigned(s[0]) << 24) | (unsigned(s[1]) << 16) | (unsigned(s[2]) << 8) | s[3];
  std::printf("%u %u\n", p, (unsigned(s[4]) << 8) | s[5]);
}
int main() {
  uint8_t s[16] = {};
  read(s, 7, 0x0010);          // no request: untouched
  fable2::f6close::Request();
  read(s, 7, 0x0010);          // B down, new packet
  read(s, 7, 0x0010);          // still down
  read(s, 7, 0x0010);          // released, new packet
  read(s, 8, 0x0000);          // real change keeps counting up
}
'''


@unittest.skipUnless(shutil.which('xcrun') or shutil.which('c++'), 'needs a C++ compiler')
class CloseTapTests(unittest.TestCase):
    def test_tap(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'tap.cpp'
            exe = Path(tmp) / 'tap'
            src.write_text(PROGRAM)
            cxx = ['xcrun', 'clang++'] if shutil.which('xcrun') else ['c++']
            subprocess.run(cxx + ['-std=c++17', '-I', str(ROOT / 'src/core'), str(src), '-o', str(exe)],
                           check=True)
            rows = [tuple(map(int, line.split())) for line in
                    subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split('\n')
                    if line]
        b = 0x2000
        self.assertEqual(rows[0], (7, 0x0010))
        self.assertEqual(rows[1], (8, 0x0010 | b))
        self.assertEqual(rows[2], (8, 0x0010 | b))
        self.assertEqual(rows[3], (9, 0x0010))
        self.assertEqual(rows[4], (10, 0x0000))


class MenuScriptTests(unittest.TestCase):
    def read(self, name):
        return (LUA / name).read_text()

    def test_no_fixed_settle_loops(self):
        for name in ['F6.lua'] + SUBSCRIPTS:
            text = self.read(name)
            self.assertNotRegex(text, r'for\s+_\s*=\s*1\s*,\s*30\s+do', name)
            code = '\n'.join(line.split('--', 1)[0] for line in text.splitlines())
            if name != 'F6.lua':
                self.assertNotIn('GUI.DisplayMenuBox', code, name)  # boxes go through st.box

    def test_subscripts_use_f6_state_and_unwind(self):
        for name in SUBSCRIPTS:
            text = self.read(name)
            self.assertIn('rawget(_G, "__f6_menu_state")', text, name)
            self.assertNotIn('__f5_menu_state', text, name)
            self.assertIn('st.box(', text, name)
            self.assertRegex(text, r'if choice == nil then return', name)

    def test_page_flip_in_slot_one(self):
        self.assertIn('local entries, map = { "More..." }, {}', self.read('character_editor.lua'))
        self.assertIn('local page = { "Next" }', self.read('modifyMoney.lua'))

    def test_settle_values(self):
        text = self.read('F6.lua')
        full = int(re.search(r'local SETTLE_FULL = (\d+)', text).group(1))
        fast = int(re.search(r'local SETTLE_FAST = (\d+)', text).group(1))
        self.assertGreaterEqual(full, 8)  # 7 ticks measured clean of the stale highlight
        self.assertGreaterEqual(fast, 2)  # 1 tick drops the box
        self.assertLess(fast, full)

    def test_close_ack_matches_host(self):
        self.assertTrue((LUA / 'F6_close_box.lua').is_file())
        self.assertIn('"scripts/recomp/F6_close_box.lua"', self.read('F6.lua'))
        header = (ROOT / 'src/core/fable2_f6_close.h').read_text()
        self.assertIn('kCloseBoxAck = "F6_close_box.lua"', header)
        self.assertIn('fable2::f6close::kCloseBoxAck', (ROOT / 'src/core/fable2_f5_lua.h').read_text())
        self.assertIn('inject_close_tap', (ROOT / 'src/diagnostics/fable2_state_probe.h').read_text())

    def test_f6_press_toggles(self):
        text = self.read('F6.lua')
        press = text[text.index('-- This press.'):text.index('if not st.registered then')]
        self.assertIn('st.closing = true', press)
        self.assertIn('pcall(RunScript, CLOSE_BOX)', press)

    def test_menu_f_key_not_gated_by_overlay(self):
        # The script menu is on 6 and F7 (2026-10-22). Number keys go through NumberRowAction, which is skipped while
        # an overlay has the keyboard; F keys go through FKeyAction, which never is (both key handlers).
        text = (ROOT / 'src/input/window_keyboard_mouse.h').read_text()
        fkeys = text[text.index('static int FKeyAction('):]
        self.assertIn('case K::kF7: return 6;', fkeys[:fkeys.index('default:')])
        self.assertEqual(text.count('overlay_captures_input ? 0 : NumberRowAction(key);'), 2)
        self.assertEqual(text.count('if (!action) action = FKeyAction(key);'), 2)


if __name__ == '__main__':
    unittest.main()
