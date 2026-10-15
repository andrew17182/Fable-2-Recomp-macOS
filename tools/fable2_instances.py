#!/usr/bin/env python3
"""Registry of running Fable II game instances (multi-instance safety).

Every launch through tools/fable2_dev_session.py or tools/fable2_test_control.py registers its
game process in out/instances/<pid>.json:
  pid, role (playtest | play | agent), build (playtest | latest | other), app, version,
  start_unix, process start time (pid-reuse check), session, test_control, settings (fps/res env)
Stale entries (process gone, or the pid now belongs to another process) are pruned on every read.
fable_2 processes that are running but not registered (started by hand, older tools) are reported
with role "untracked".

  python3 tools/fable2_instances.py            # JSON list of running instances
  python3 tools/fable2_instances.py --text     # one line per instance + contention summary

Perf rule: numbers measured while another instance runs are not comparable (shared GPU/CPU);
contention() gives the flag that f2diag prints. Runs under /usr/bin/python3 (3.9).
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import time
import uuid

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402  (main checkout vs agent slot paths)

ROOT = common.ROOT  # this checkout (main or an agent slot)
# Global across the main checkout and every agent slot (local agent docs): contention detection must
# see the games of all slots, the playtest copy and the player's games.
REGISTRY = common.MAIN_ROOT / 'out/instances'
APP_REL = 'out/build/mac-arm64-metal-release/Fable II.app'
LATEST_APP = ROOT / APP_REL  # this checkout's own build
PLAYTEST_DIR = common.MAIN_ROOT / 'out/playtest'  # the playtest copy lives in the main checkout only
PLAYTEST_APP = PLAYTEST_DIR / 'Fable II.app'
# Settings worth showing next to a perf number (what the other instance was doing).
SETTING_KEYS = ('FABLE2_FPS_60', 'REX_VSYNC', 'FABLE2_RES_SCALE', 'FABLE2_UPSCALING', 'FABLE2_METALFX', 'FABLE2_FULLSCREEN',
                'FABLE2_HEAVY_RUN_ID')  # heavy-tool run that launched it (tools/fable2_heavy.py)


def game_pids():
    result = subprocess.run(['pgrep', '-x', 'fable_2'], capture_output=True, text=True)
    if result.returncode not in (0, 1): raise RuntimeError('pgrep failed: ' + result.stderr.strip())
    return [int(p) for p in result.stdout.split()]


def process_info(pid):
    """dict(exe, lstart) of a live, non-zombie process, else None. exe is the full executable path."""
    pid = int(pid)
    result = subprocess.run(['ps', '-ww', '-p', str(pid), '-o', 'stat=,lstart=,comm='], capture_output=True, text=True)
    line = result.stdout.strip()
    if result.returncode != 0 or not line: return None
    parts = line.split(None, 6)  # stat, lstart (5 words: "Sat Oct 10 14:01:02 2026"), comm (may hold spaces)
    if len(parts) < 7 or parts[0].startswith('Z'): return None
    return dict(exe=parts[6], lstart=' '.join(parts[1:6]))


def _inside(path, app):
    return path == str(app) or path.startswith(str(app) + '/')


def build_of(exe_or_app):
    """'playtest' | 'latest' (the main checkout's build) | 'agent-N' (an agent slot's build) | 'other'."""
    path = str(exe_or_app)
    if _inside(path, PLAYTEST_APP): return 'playtest'
    if _inside(path, common.MAIN_ROOT / APP_REL): return 'latest'
    if _inside(path, LATEST_APP): return common.SLOT or 'latest'
    for name, root in common.checkouts():
        if _inside(path, root / APP_REL): return 'latest' if name == 'main' else name
    return 'other'


def app_version(app):
    try:
        with open(Path(app) / 'Contents/Info.plist', 'rb') as f:
            return plistlib.load(f).get('Fable2VersionString')
    except (OSError, ValueError):
        return None


def _atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp-' + uuid.uuid4().hex[:8])
    tmp.write_text(json.dumps(data, indent=2) + '\n')
    os.replace(tmp, path)


def register(pid, role, app, session=None, test_control=False, env=None, state=None):
    info = process_info(pid) or {}
    env = env or {}
    entry = dict(pid=int(pid), role=role, build=build_of(app), app=str(app), version=app_version(app),
                 start_unix=round(time.time(), 3), lstart=info.get('lstart'), exe=info.get('exe'),
                 session=str(session) if session else None, state=str(state) if state else None,
                 checkout=common.SLOT or 'main',
                 test_control=bool(test_control),
                 input='test-only' if env.get('FABLE2_PHYSICAL_INPUT') == '0' else 'physical',
                 settings={k: env[k] for k in SETTING_KEYS if k in env})
    _atomic_write(REGISTRY / f'{int(pid)}.json', entry)
    return entry


def unregister(pid):
    try:
        (REGISTRY / f'{int(pid)}.json').unlink()
    except FileNotFoundError:
        pass


def instances():
    """Running instances (registry entries whose process is alive + untracked fable_2 processes)."""
    out, seen = [], set()
    for path in sorted(REGISTRY.glob('*.json')) if REGISTRY.is_dir() else []:
        try:
            entry = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        info = process_info(entry.get('pid', 0))
        if (info is None or Path(info['exe']).name != 'fable_2' or
                (entry.get('lstart') and info['lstart'] != entry['lstart'])):
            path.unlink(missing_ok=True)  # exited, or the pid was reused
            continue
        seen.add(entry['pid'])
        out.append(entry)
    for pid in game_pids():
        if pid in seen: continue
        info = process_info(pid)
        if info is None: continue
        out.append(dict(pid=pid, role='untracked', build=build_of(info['exe']), exe=info['exe'], input='unknown',
                        lstart=info['lstart'], version=None, session=None, test_control=None, settings={}))
    out.sort(key=lambda e: e['pid'])
    return out


def describe(entry):
    text = f"{entry['role']} pid {entry['pid']}"
    extra = [entry.get('build') or '?']
    if entry.get('version'): extra.append(entry['version'])
    settings = entry.get('settings') or {}
    if settings.get('FABLE2_RES_SCALE') not in (None, '1'): extra.append('res ' + ('auto' if settings['FABLE2_RES_SCALE'] == 'auto' else 'x' + settings['FABLE2_RES_SCALE']))
    if settings.get('FABLE2_FPS_60') is not None:
        extra.append('uncapped' if settings.get('REX_VSYNC') == '0' else
                     {'1': '60fps', '0': '30fps'}.get(settings['FABLE2_FPS_60'], 'fps ' + settings['FABLE2_FPS_60']))
    return text + ' (' + ', '.join(extra) + ')'


def contention(own_pid=None, current=None):
    """Other instances than own_pid: dict(count, others=[entries], text). text is '' when alone."""
    current = instances() if current is None else current
    others = [e for e in current if own_pid is None or e['pid'] != int(own_pid)]
    return dict(count=len(others), others=others, text=contention_text(others))


def contention_text(others):
    if not others: return ''
    return ('CONTENTION: %d other instance%s running (%s) — perf numbers not comparable'
            % (len(others), '' if len(others) == 1 else 's', '; '.join(describe(e) for e in others)))


def tag_value(others):
    """Flight-recorder 'contention' tag text: '0' or 'N: playtest 123, agent 456'."""
    if not others: return '0'
    return '%d: %s' % (len(others), ', '.join(f"{e['role']} {e['pid']}" for e in others))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--text', action='store_true')
    args = ap.parse_args()
    current = instances()
    if args.text:
        for e in current: print(describe(e) + (f"  session {Path(e['session']).name}" if e.get('session') else ''))
        if not current: print('no Fable II instance running')
    else:
        print(json.dumps(current, indent=2))


if __name__ == '__main__':
    main()
