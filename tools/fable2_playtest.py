#!/usr/bin/env python3
"""Frozen playtest copy of the game (local agent docs).

  python3 tools/fable2_playtest.py update    # freeze the latest build as the playtest copy
  python3 tools/fable2_playtest.py status    # what the playtest copy was frozen from + running instances

The playtest copy lives in out/playtest/:
  Fable II.app   copy of out/build/mac-arm64-metal-release/Fable II.app, re-signed with its own bundle
                 id (org.fable2recomp.metal.playtest, name "Fable II Playtest"), so macOS keeps its
                 Metal compiler cache and window state apart from the agents' build
  game-data/     default.xex + data/ (+ nxeart) as APFS clones of the checkout's game data (no extra disk space).
                 The game writes data/scripts/recomp/recomp_notify_text.lua at runtime and every build
                 restages data/scripts/recomp + data/dir.manifest, so the playtest copy must not share
                 the checkout's data/ folder with agent builds. game-data/Screenshots links to the
                 checkout's Screenshots/ folder (controller View button).
  playtest.json  version string, git sha, source build hash, freeze time
Agent builds only replace out/build/...; they never touch out/playtest. Saves, settings and the
game's own pipeline archive stay in the live state folder (Application Support), which only
normal play sessions write; agent test sessions use copies.
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_instances as instances  # noqa: E402

ROOT = instances.ROOT
LATEST_APP = instances.LATEST_APP
PLAYTEST_DIR = instances.PLAYTEST_DIR
PLAYTEST_APP = instances.PLAYTEST_APP
PLAYTEST_DATA = PLAYTEST_DIR / 'game-data'
PLAYTEST_INFO = PLAYTEST_DIR / 'playtest.json'
BUNDLE_SUFFIX = '.playtest'
DISPLAY_NAME = 'Fable II Playtest'
GAME_DATA_ITEMS = ('default.xex', 'data', 'nxeart')  # read-only game content (APFS clones)
# Read through the game root at runtime too: skip intro redirects the logo movies to
# game:\out\skipintro\*.bik (src/core/fable2_skip_intro.h), so without them the intros play.
GAME_ROOT_EXTRAS = ('out/skipintro',)


def app_for(build):
    if build == 'playtest': return PLAYTEST_APP
    if build == 'latest': return LATEST_APP
    raise ValueError('build must be playtest or latest')


def data_root_for(build):
    return PLAYTEST_DATA if build == 'playtest' else ROOT


def info():
    try:
        return json.loads(PLAYTEST_INFO.read_text())
    except (OSError, ValueError):
        return None


def _run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def update():
    if instances.common.IS_SLOT:
        raise RuntimeError('The playtest copy is updated from the main checkout only (this is agent slot %s)'
                           % instances.common.SLOT)
    if not LATEST_APP.is_dir(): raise RuntimeError(f'No build to freeze: {LATEST_APP.relative_to(ROOT)}')
    running = [e for e in instances.instances() if e.get('build') == 'playtest']
    if running:
        raise RuntimeError('The playtest copy is running (%s); close it before updating'
                           % ', '.join(instances.describe(e) for e in running))
    for item in ('default.xex', 'data'):
        if not (ROOT / item).exists(): raise RuntimeError(f'Game content missing: {item}')
    started = time.monotonic()
    PLAYTEST_DIR.mkdir(parents=True, exist_ok=True)
    work = PLAYTEST_DIR / ('.update-' + uuid.uuid4().hex[:8])
    work.mkdir()
    try:
        app = work / PLAYTEST_APP.name
        _run('ditto', LATEST_APP, app)
        plist_path = app / 'Contents/Info.plist'
        with open(plist_path, 'rb') as f: plist = plistlib.load(f)
        source_id = plist.get('CFBundleIdentifier', 'org.fable2recomp.metal')
        plist['CFBundleIdentifier'] = source_id + BUNDLE_SUFFIX
        plist['CFBundleName'] = plist['CFBundleDisplayName'] = DISPLAY_NAME
        plist['Fable2Playtest'] = True
        with open(plist_path, 'wb') as f: plistlib.dump(plist, f)
        defaults = app / 'Contents/Resources/LocalDefaults.plist'
        with open(defaults, 'wb') as f: plistlib.dump({'GameDataRoot': str(PLAYTEST_DATA)}, f)
        _run('codesign', '--force', '--sign', '-', app)
        _run('codesign', '--verify', '--deep', '--strict', app)
        data = work / PLAYTEST_DATA.name
        data.mkdir()
        for item in GAME_DATA_ITEMS:
            if (ROOT / item).exists():
                _run('cp', '-cR', ROOT / item, data / item)  # clonefile: instant, shares blocks
        for item in GAME_ROOT_EXTRAS:
            if not (ROOT / item).is_dir(): raise RuntimeError(f'Missing {item} (needed for skip intro)')
            (data / item).parent.mkdir(parents=True, exist_ok=True)
            _run('cp', '-cR', ROOT / item, data / item)
        # View-button screenshots go to <game data>/Screenshots: keep them in the checkout's folder.
        (ROOT / 'Screenshots').mkdir(exist_ok=True)
        (data / 'Screenshots').symlink_to(Path('../../..') / 'Screenshots')
        record = dict(version=plist.get('Fable2VersionString'), bundle_id=plist['CFBundleIdentifier'],
                      frozen_at=time.strftime('%Y-%m-%d %H:%M:%S'),
                      source=str(LATEST_APP.relative_to(ROOT)),
                      fable_2_sha256=_sha256(app / 'Contents/MacOS/fable_2'))
        # Swap in: old copy aside first, then the new one, then delete the old one.
        old = work / 'old'
        old.mkdir()
        for target, new in ((PLAYTEST_APP, app), (PLAYTEST_DATA, data)):
            if target.exists(): target.rename(old / target.name)
            new.rename(target)
        (PLAYTEST_INFO.with_suffix('.tmp')).write_text(json.dumps(record, indent=2) + '\n')
        (PLAYTEST_INFO.with_suffix('.tmp')).replace(PLAYTEST_INFO)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    record['seconds'] = round(time.monotonic() - started, 1)
    return record


def status():
    return dict(playtest=info(), installed=PLAYTEST_APP.is_dir(),
                latest_version=instances.app_version(LATEST_APP),
                running=[instances.describe(e) for e in instances.instances()])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['update', 'status'])
    args = ap.parse_args()
    try:
        result = update() if args.cmd == 'update' else status()
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
