#!/usr/bin/python3
"""Watchdog for a capture run: watch RSS of this slot's game and free disk; close by session if exceeded."""
import json, os, subprocess, sys, time, pathlib, shutil
SLOT = pathlib.Path(__file__).resolve().parents[2]  # this checkout (tools/shaderlab/..)
sys.path.insert(0, str(SLOT / 'tools'))
import fable2_common as common  # noqa: E402
MAIN = common.MAIN_ROOT
LIM_GB = float(sys.argv[1]) if len(sys.argv) > 1 else 16.0
DISK_GB = 150.0
WORK = pathlib.Path(os.environ.get('SHADERLAB_DIR', str(SLOT / 'out/shaderlab')))
WORK.mkdir(parents=True, exist_ok=True)
log = open(WORK / 'watchdog.log', 'a')
seen = None; peak = 0.0; t0 = time.time()
while time.time() - t0 < 600:
    ent = None
    for p in (MAIN / 'out/instances').glob('*.json'):
        try: e = json.loads(p.read_text())
        except Exception: continue
        if e.get('checkout') == SLOT.name:
            ent = e
    if ent is None:
        if seen: break
        time.sleep(0.5); continue
    if seen is None:
        seen = ent; print('entry', json.dumps(ent), file=log, flush=True)
    pid = ent['pid']
    r = subprocess.run(['ps', '-o', 'rss=', '-p', str(pid)], capture_output=True, text=True).stdout.strip()
    if not r: break
    rss = int(r) / 1048576; peak = max(peak, rss)
    free = shutil.disk_usage(str(SLOT)).free / 1e9
    print(f'{time.time()-t0:6.1f}s rss {rss:.2f} GB free {free:.0f} GB', file=log, flush=True)
    if rss > LIM_GB or free < DISK_GB:
        sess = ent.get('session') or ent.get('session_dir') or ''
        print('LIMIT EXCEEDED, closing', sess, file=log, flush=True)
        subprocess.run(['/usr/bin/python3', str(SLOT / 'tools/fable2_test_control.py'), 'close', pathlib.Path(sess).name], cwd=SLOT)
        break
    time.sleep(0.5)
print(f'done peak rss {peak:.2f} GB', file=log, flush=True)
