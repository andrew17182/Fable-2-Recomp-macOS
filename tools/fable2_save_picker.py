"""Readable save-library entries for the Dev Launcher's save picker (runs under /usr/bin/python3 3.9).

`tools/fable2_dev_session.py list` passes every library entry through annotate(), which adds:
  place, quest      friendly names for LevelName / QuestName (table below, else the code de-camel-cased)
  title             "Place · Quest" (the launcher appends the date)
  group             entries with the same (LevelName, QuestName, hero folder) share a group; the picker shows the
                    newest of each group and folds the rest under "N older"
  test, test_reason saves made by agent test sessions (hidden by default behind "Show test saves")

A save is a test save when its session's role is `agent`: from <workspace>/agent-metrics/game-sessions.jsonl
(2026-10-11 on), else the session folder's session.json (role / test_control) while it still exists. Older sessions
are pruned and were never logged, so the fallback is the write-back: test sessions never write back to a live slot,
while the launcher's play sessions write back to the entry's own HeroNNN by default. Imported entries (no session)
are never test saves.

Stars and custom titles are metadata only: <main checkout>/out/save-library/.picker.json, written by the launcher
(save folders are never touched). `python3 tools/fable2_save_picker.py` prints the annotated library.
"""
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402

# Last component of LevelName (e.g. Caves\BowerLake\ThagsCave). The save carries no finer region than this.
LEVEL_NAMES = {
    'BowerLake': 'Bower Lake',
    'BWSSlums': 'Bowerstone Slums',
    'BWSMarket': 'Bowerstone Market',
    'BWSOldTown': 'Bowerstone Old Town',
    'BWSCastle': 'Bowerstone Castle',
    'ThagsCave': "Heroes' Guild (Thag's Cave)",
    'Rookridge': 'Rookridge',
    'OakfieldVillage': 'Oakfield',
    'Westcliff': 'Westcliff',
    'Bloodstone': 'Bloodstone',
    'WraithmarshVillage': 'Wraithmarsh',
}
QUEST_NAMES = {
    'QC010_Childhood': 'Childhood',
    'QC040_BulliesAndDog': 'Bullies and the Dog',
    'QC045_LoveLetter': 'Love Letter',
    'QC050_Wanted': 'Wanted',
    'QC055_Wino': 'The Wino',
    'QC060_NewBeginning': 'Birth of a Hero',
    'QC070_Thag': 'Thag',
    'QC075_JourneyToRavenscar': 'Journey to Ravenscar',
}
SMALL_WORDS = {'And', 'To', 'Of', 'The', 'In', 'On', 'At', 'A', 'For'}


def decamel(code):
    """'JourneyToRavenscar' -> 'Journey to Ravenscar', 'BWSSlums' -> 'BWS Slums'."""
    words = re.findall(r'[A-Z]+(?=[A-Z][a-z]|\d|\b)|[A-Z]?[a-z]+|\d+|[A-Z]+', code or '')
    out = [w.lower() if i and w in SMALL_WORDS else w for i, w in enumerate(words)]
    return ' '.join(out) or (code or '')


def friendly_level(level):
    if not level: return ''
    last = re.split(r'[\\/]', level)[-1]
    return LEVEL_NAMES.get(last) or decamel(last)


def friendly_quest(quest):
    if not quest: return ''
    if quest in QUEST_NAMES: return QUEST_NAMES[quest]
    return decamel(re.sub(r'^Q[A-Z]*\d+_', '', quest))


def group_key(entry):
    return '|'.join((entry.get('LevelName') or '', entry.get('QuestName') or '', entry.get('folder') or ''))


def session_roles(metrics=None, session_dirs=None):
    """{session: 'agent'|'play'|'playtest'} from the agent-metrics game log and session folders still on disk."""
    roles = {}
    metrics = Path(metrics) if metrics else common.AGENT_METRICS / 'game-sessions.jsonl'
    try:
        lines = metrics.read_text().splitlines()
    except OSError:
        lines = []
    for line in lines:
        try:
            item = json.loads(line)
        except ValueError:
            continue
        if isinstance(item, dict) and item.get('session') and item.get('role'):
            roles[item['session']] = item['role']
    if session_dirs is None:
        session_dirs = {common.ROOT / 'out/dev-sessions', common.MAIN_ROOT / 'out/dev-sessions'}
    for folder in session_dirs:
        for record in Path(folder).glob('*/session.json') if Path(folder).is_dir() else []:
            if record.parent.name in roles: continue
            try:
                data = json.loads(record.read_text())
            except (OSError, ValueError):
                continue
            role = data.get('role') or ('agent' if data.get('test_control') else None)
            if role: roles[record.parent.name] = role
    return roles


def classify(entry, roles):
    """(test, reason) for one library entry (save.json fields)."""
    session = entry.get('session')
    if not session: return False, 'imported' if entry.get('imported') else 'no session'
    role = roles.get(session)
    if role: return role == 'agent', 'session role ' + role
    if entry.get('written_back_to'): return False, 'written back to ' + entry['written_back_to']
    return True, 'library-only write-back (test sessions never write to a live slot)'


def annotate(entry, roles):
    place, quest = friendly_level(entry.get('LevelName')), friendly_quest(entry.get('QuestName'))
    test, reason = classify(entry, roles)
    entry.update(place=place, quest=quest, title=' · '.join(p for p in (place, quest) if p) or entry.get('name'),
                 group=group_key(entry), test=test, test_reason=reason)
    return entry


def picker_meta_path():
    """Stars / custom titles: the main checkout's library (shared, like the saves)."""
    return common.MAIN_ROOT / 'out/save-library/.picker.json'


if __name__ == '__main__':
    import fable2_dev_session as dev  # noqa: E402
    for e in dev.library_entries():
        print(('TEST ' if e['test'] else 'play ') + e['name'] + '  ' + e['title'] + '  [' + e['test_reason'] + ']')
