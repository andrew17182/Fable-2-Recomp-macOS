"""Resolution and frame-rate "auto": the launchers' copy of the tables
(tools/macos/Shared/LaunchSettings.swift, AutoResolution and AutoFrameRate, compiled into the Dev Launcher and
the player app) must match src/core/fable2_res_auto.h, so the launchers' "Auto (N× on this Mac)" / "Auto (N FPS on this Mac)"
labels name what the game picks. Runs under /usr/bin/python3 (3.9)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / 'src/core/fable2_res_auto.h').read_text()
SWIFT = (ROOT / 'tools/macos/Shared/LaunchSettings.swift').read_text()
SWIFT_TABLES = SWIFT[SWIFT.index('// BEGIN res-auto tables'):SWIFT.index('// END res-auto tables')]
SWIFT_FPS = SWIFT[SWIFT.index('// BEGIN fps-auto tables'):SWIFT.index('// END fps-auto tables')]


def pairs(text):
    return [(int(a), float(b)) for a, b in re.findall(r'\{?\(?\s*(\d+),\s*([\d.]+)\s*[}\)]', text)]


class ResAutoSyncTests(unittest.TestCase):
    def test_generation_factors(self):
        cpp = HEADER[HEADER.index('kGenerationFactors[] = {'):]
        cpp = cpp[:cpp.index('};')]
        swift = SWIFT_TABLES[SWIFT_TABLES.index('generationFactors'):SWIFT_TABLES.index('ultraEfficiency')]
        self.assertEqual(pairs(cpp), pairs(swift))
        self.assertTrue(pairs(cpp))

    def test_scale_rows(self):
        rows = r'\{?\(?\s*(\d+),\s*([\d.]+),\s*([\d.]+)\s*[}\)]'
        cpp = HEADER[HEADER.index('kScaleRows[] = {'):]
        cpp = cpp[:cpp.index('};')]
        swift = SWIFT_TABLES[SWIFT_TABLES.index('scaleRows'):SWIFT_TABLES.index('minRamGb2x')]
        norm = lambda t: [(int(a), float(b), float(c)) for a, b, c in re.findall(rows, t)]
        self.assertEqual(norm(cpp), norm(swift))
        self.assertEqual(len(norm(cpp)), 2)

    def test_scalars(self):
        for cpp_name, swift_name in (('kUltraEfficiency', 'ultraEfficiency'), ('kMinRamGb2x', 'minRamGb2x'),
                                     ('kMinRamGb3x', 'minRamGb3x')):
            a = re.search(cpp_name + r'\s*=\s*([\d.]+)', HEADER).group(1)
            b = re.search(swift_name + r'\s*=\s*([\d.]+)', SWIFT_TABLES).group(1)
            self.assertEqual(float(a), float(b), cpp_name)
        a = re.search(r'kFallbackCores\[\]\s*=\s*\{([^}]*)\}', HEADER).group(1)
        a = [int(x) for x in re.findall(r'(?<![*\w])(\d+)(?![*\w])', re.sub(r'/\*.*?\*/', '', a))]
        b = [int(x) for x in re.search(r'fallbackCores\s*=\s*\[([^\]]*)\]', SWIFT_TABLES).group(1).split(',')]
        self.assertEqual(a, b)

    def test_frame_model(self):
        rows = r'\{?\(?\s*(\d+),\s*(-?[\d.]+),\s*([\d.]+)\s*[}\)]'
        cpp = HEADER[HEADER.index('kFrameModel[] = {'):]
        cpp = cpp[:cpp.index('};')]
        swift = SWIFT_FPS[SWIFT_FPS.index('frameModel'):SWIFT_FPS.index('maxMeanMs')]
        norm = lambda t: [(int(a), float(b), float(c)) for a, b, c in re.findall(rows, t)]
        self.assertEqual(norm(cpp), norm(swift))
        self.assertEqual([r[0] for r in norm(cpp)], [1, 2, 3])
        a = re.search(r'kFpsAutoMaxMeanMs\s*=\s*([\d.]+)', HEADER).group(1)
        b = re.search(r'maxMeanMs\s*=\s*([\d.]+)', SWIFT_FPS).group(1)
        self.assertEqual(float(a), float(b))
        a = re.search(r'kBaseTierMaxAutoFps\s*=\s*(\d+)', HEADER).group(1)
        b = re.search(r'baseTierMaxAutoFps\s*=\s*(\d+)', SWIFT_FPS).group(1)
        self.assertEqual(int(a), int(b))
        a = re.search(r'kUncapped2xMaxMeanMs\s*=\s*([\d.]+)', HEADER).group(1)
        b = re.search(r'uncapped2xMaxMeanMs\s*=\s*([\d.]+)', SWIFT_FPS).group(1)
        self.assertEqual(float(a), float(b))


class AutoChoiceTests(unittest.TestCase):
    """The shared tables, evaluated the way both copies do (ChooseFrameTarget / ChooseBoth in the header,
    AutoFrameRate.choose / both in Swift), for the Macs the 2x rule was written for."""

    @classmethod
    def setUpClass(cls):
        cpp = HEADER[HEADER.index('kFrameModel[] = {'):]
        cpp = cpp[:cpp.index('};')]
        cls.model = {int(s): (float(f), float(w)) for s, f, w in
                     re.findall(r'\{\s*(\d+),\s*(-?[\d.]+),\s*([\d.]+)\s*\}', cpp)}
        g = HEADER[HEADER.index('kGenerationFactors[] = {'):]
        cls.factors = dict(pairs(g[:g.index('};')]))
        r = HEADER[HEADER.index('kScaleRows[] = {'):]
        cls.rows = {int(t): (float(a), float(b)) for t, a, b in
                    re.findall(r'\{(\d+),\s*([\d.]+),\s*([\d.]+)\}', r[:r.index('};')])}
        num = lambda name: float(re.search(name + r'\s*=\s*([\d.]+)', HEADER).group(1))
        cls.max60, cls.unc, cls.ram2 = num('kFpsAutoMaxMeanMs'), num('kUncapped2xMaxMeanMs'), num('kMinRamGb2x')

    def effective(self, gen, cores, ultra=False):
        return cores * self.factors[gen] * (0.75 if ultra else 1.0)

    def fps(self, eff, scale, base=False):
        f, w = self.model[scale]
        est = f + w / eff
        if base:
            return 30  # base-tier rule (kBaseTierMaxAutoFps)
        return 60 if est <= self.max60 else (0 if scale == 2 and est <= self.unc else 30)

    def both(self, eff, ram, base=False):
        fps = self.fps(eff, 1, base)
        need2, need3 = self.rows[60 if fps > 30 else 30]
        scale = 3 if eff >= need3 and ram >= 32 else 2 if eff >= need2 and ram >= self.ram2 else 1
        if scale < 2 and ram >= self.ram2 and self.fps(eff, 2, base) != 30:
            return 2, self.fps(eff, 2, base)
        return scale, fps

    def test_m3_pro_gets_2x_uncapped(self):
        self.assertEqual(self.both(self.effective(3, 14), 36), (2, 0))

    def test_m5_max_stays_2x_60(self):
        self.assertEqual(self.both(self.effective(5, 40), 64), (2, 60))

    def test_base_m1_not_2x(self):
        for cores, ram in ((7, 8), (8, 16)):
            self.assertEqual(self.both(self.effective(1, cores), ram, base=True), (1, 30))
            f, w = self.model[2]
            self.assertLess(1000 / (f + w / self.effective(1, cores)), 40)  # it would be under 40 FPS at 2x

    def test_base_chips_stay_at_30(self):
        for gen in range(1, 6):
            for cores in (8, 10):
                self.assertEqual(self.both(self.effective(gen, cores), 16, base=True), (1, 30))

    def test_m2_pro(self):
        self.assertEqual(self.both(self.effective(2, 16), 16), (2, 0))
        self.assertEqual(self.both(self.effective(2, 19), 32), (2, 60))

    def test_uncapped_band_is_45_fps_or_more(self):
        self.assertLessEqual(self.unc, 1000 / 45.0)
        self.assertGreater(self.unc, self.max60)


if __name__ == '__main__':
    unittest.main()
