"""Dev Launcher save picker: friendly names, test-save classification (tools/fable2_save_picker.py) and the
picker's grouping / stars / search (tools/macos/DevLauncher/SavePicker.swift via --picker-rows).

Uses made-up entries and temp files only (never the real library). The Swift part builds the launcher with swiftc
(skipped without Xcode). Runs under /usr/bin/python3 (3.9)."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fable2_save_picker as picker  # noqa: E402


def entry(name, level, quest, created, session='s-play', written_back_to='Hero000', folder='Hero000', **extra):
    data = dict(name=name, LevelName=level, QuestName=quest, created=created, session=session,
                written_back_to=written_back_to, folder=folder, header_display_name='Hero 1', Chapter='2.100000')
    data.update(extra)
    return data


class NameTests(unittest.TestCase):
    def test_levels(self):
        self.assertEqual(picker.friendly_level('BowerLake'), 'Bower Lake')
        self.assertEqual(picker.friendly_level('BWSSlums'), 'Bowerstone Slums')
        self.assertEqual(picker.friendly_level('Caves\\BowerLake\\ThagsCave'), "Heroes' Guild (Thag's Cave)")
        self.assertEqual(picker.friendly_level('Caves\\Somewhere\\WraithMarshCrypt'), 'Wraith Marsh Crypt')
        self.assertEqual(picker.friendly_level(None), '')

    def test_quests(self):
        self.assertEqual(picker.friendly_quest('QC075_JourneyToRavenscar'), 'Journey to Ravenscar')
        self.assertEqual(picker.friendly_quest('QC010_Childhood'), 'Childhood')
        self.assertEqual(picker.friendly_quest('QC060_NewBeginning'), 'Birth of a Hero')
        self.assertEqual(picker.friendly_quest('QC120_TheHeroOfStrength'), 'The Hero of Strength')

    def test_decamel(self):
        self.assertEqual(picker.decamel('BWSOldTown'), 'BWS Old Town')
        self.assertEqual(picker.decamel('Level2Gate'), 'Level 2 Gate')


class ClassifyTests(unittest.TestCase):
    def test_roles_and_fallbacks(self):
        roles = {'s-agent': 'agent', 's-play': 'play', 's-playtest': 'playtest'}
        cases = [
            (entry('a', 'BowerLake', 'QC070_Thag', '2026-10-11 10:00:00', session='s-agent', written_back_to=None), True),
            (entry('b', 'BowerLake', 'QC070_Thag', '2026-10-11 10:00:00', session='s-play', written_back_to=None), False),
            (entry('c', 'BowerLake', 'QC070_Thag', '2026-10-11 10:00:00', session='s-playtest'), False),
            # Session pruned and never logged: library-only write-back means a test session.
            (entry('d', 'BowerLake', 'QC070_Thag', '2026-10-05 10:00:00', session='gone', written_back_to=None), True),
            (entry('e', 'BowerLake', 'QC070_Thag', '2026-10-05 10:00:00', session='gone'), False),
            (dict(name='f', imported='2026-10-01 07:00:00', LevelName='BWSSlums'), False),
        ]
        for data, test in cases:
            self.assertEqual(picker.classify(data, roles)[0], test, data['name'])

    def test_session_roles(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            log = temp / 'game-sessions.jsonl'
            log.write_text(json.dumps(dict(session='s1', role='agent')) + '\nnot json\n'
                           + json.dumps(dict(session='s2', role='play')) + '\n')
            sessions = temp / 'dev-sessions'
            (sessions / 's3').mkdir(parents=True)
            (sessions / 's3/session.json').write_text(json.dumps(dict(test_control=True)))
            (sessions / 's2').mkdir()
            (sessions / 's2/session.json').write_text(json.dumps(dict(role='agent')))  # the log wins
            roles = picker.session_roles(log, [sessions])
            self.assertEqual(roles, {'s1': 'agent', 's2': 'play', 's3': 'agent'})

    def test_annotate(self):
        data = picker.annotate(entry('x', 'Caves\\BowerLake\\ThagsCave', 'QC060_NewBeginning', '2026-10-07 14:45:08'), {})
        self.assertEqual(data['title'], "Heroes' Guild (Thag's Cave) · Birth of a Hero")
        self.assertEqual(data['group'], 'Caves\\BowerLake\\ThagsCave|QC060_NewBeginning|Hero000')
        self.assertFalse(data['test'])


def library():
    """Newest first, as tools/fable2_dev_session.py list returns it."""
    rows = [
        entry('guild-3', 'Caves\\BowerLake\\ThagsCave', 'QC060_NewBeginning', '2026-10-08 09:00:00'),
        entry('lake-test', 'BowerLake', 'QC070_Thag', '2026-10-08 08:30:00', written_back_to=None, session='gone'),
        entry('guild-2', 'Caves\\BowerLake\\ThagsCave', 'QC060_NewBeginning', '2026-10-07 14:45:08'),
        entry('lake-1', 'BowerLake', 'QC070_Thag', '2026-10-07 10:00:00'),
        entry('guild-1', 'Caves\\BowerLake\\ThagsCave', 'QC060_NewBeginning', '2026-10-07 09:00:00'),
        entry('guild-hero2', 'Caves\\BowerLake\\ThagsCave', 'QC060_NewBeginning', '2026-10-06 09:00:00',
              folder='Hero001', header_display_name='Hero 2'),
        dict(name='slums', imported='2026-10-01 07:00:00', LevelName='BWSSlums', QuestName='QC010_Childhood',
             folder='Hero000', header_display_name='Hero 1', note='childhood import'),
    ]
    return [picker.annotate(r, {}) for r in rows]


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode (swiftc)')
class PickerRowsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.app = cls.root / 'SavePickerTestLauncher'
        # The one app's sources (player entry point + Dev Launcher + shared; --picker-rows works in both roles).
        sources = [ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift')) \
            + sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))
        subprocess.run(['xcrun', 'swiftc', '-target', 'arm64-apple-macos15.0', '-o', str(cls.app)]
                       + [str(s) for s in sources], check=True)
        cls.list = cls.root / 'list.json'
        cls.meta = cls.root / 'picker.json'
        cls.list.write_text(json.dumps(dict(ok=True, result=dict(live=[], library=library(),
                                                                   picker_meta=str(cls.meta)))))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def rows(self, *args, meta=None):
        self.meta.write_text(json.dumps(meta or dict(version=1, entries={})))
        out = subprocess.run([str(self.app), '--picker-rows', str(self.list)] + list(args),
                             capture_output=True, text=True, check=True).stdout
        return json.loads(out)

    def test_grouping_newest_first(self):
        rows = self.rows()
        self.assertEqual([g['newest']['name'] for g in rows['groups']], ['guild-3', 'lake-1', 'guild-hero2', 'slums'])
        self.assertEqual([r['name'] for r in rows['groups'][0]['older']], ['guild-2', 'guild-1'])
        self.assertEqual(rows['hidden_tests'], 1)
        self.assertEqual(rows['groups'][0]['newest']['title'], "Heroes' Guild (Thag's Cave) · Birth of a Hero · Oct 8, 9:00 am")
        self.assertTrue(rows['groups'][0]['newest']['detail'].startswith('Hero 1 · Hero000 · Chapter 2.1'))

    def test_show_tests(self):
        rows = self.rows('--show-tests')
        self.assertEqual(rows['hidden_tests'], 0)
        self.assertEqual(rows['groups'][1]['newest']['name'], 'lake-test')
        self.assertTrue(rows['groups'][1]['newest']['test'])

    def test_stars_and_titles(self):
        meta = dict(version=1, entries={'guild-2': dict(starred=True, title='Heroes Guild - dog close')})
        rows = self.rows(meta=meta)
        self.assertEqual([(r['name'], r['title']) for r in rows['starred']], [('guild-2', 'Heroes Guild - dog close')])
        self.assertEqual([r['name'] for r in rows['groups'][0]['older']], ['guild-1'])  # starred row leaves its group

    def test_search(self):
        self.assertEqual([g['newest']['name'] for g in self.rows('--query', 'childhood')['groups']], ['slums'])
        self.assertEqual([g['newest']['name'] for g in self.rows('--query', 'QC070 bower')['groups']], ['lake-1'])
        meta = dict(version=1, entries={'guild-1': dict(title='fur tests')})
        rows = self.rows('--query', 'FUR', meta=meta)
        self.assertEqual([g['newest']['name'] for g in rows['groups']], ['guild-1'])


if __name__ == '__main__':
    unittest.main()
