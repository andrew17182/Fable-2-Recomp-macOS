#!/usr/bin/env python3
"""FableIIBenchmark failure policy (tools/macos/Benchmark/main.swift runArmChecked), with a fake game:
- a crash AFTER the arm's data was written keeps the data (outcome "data kept: ...");
- a crash BEFORE it is retried once (first attempt kept as raw/<run>/attempt1);
- two failures record the run as failed, the kit continues with the other arms, and the report says so;
- failed runs are left out of the flash averages.
Needs Xcode's swiftc (skips without it). Runs under /usr/bin/python3 (3.9)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/macos/Benchmark/main.swift'

FAKE_GAME = r'''#!/bin/bash
# Fake FableIILauncher: behaviour per run from FAKE_PLAN ("<run>:<mode>,..."): ok | after | before-once | always.
run="${FABLE2_WINDOW_TAG#benchmark }"
mode=ok
IFS=',' read -ra items <<< "$FAKE_PLAN"
for it in "${items[@]}"; do [ "${it%%:*}" = "$run" ] && mode="${it#*:}"; done
count_file="$FABLE2_APP_STATE_ROOT/attempts-$run"
n=$(( $(cat "$count_file" 2>/dev/null || echo 0) + 1 )); echo $n > "$count_file"
logs="$FABLE2_APP_STATE_ROOT/logs"; mkdir -p "$logs"
echo "fake game $run attempt $n mode $mode" > "$logs/fable_2.log"
crash() { kill -SEGV $$; }
if [ "$mode" = always ] || { [ "$mode" = before-once ] && [ $n = 1 ]; }; then crash; fi
{ echo "event,frame,time_us,duration_us"; for f in $(seq 0 30); do echo "guest_swap,$f,$((f*16667)),0"; done; } > "$logs/frames-1.csv"
echo '{"flash_frames": 3, "crack_frames": 5, "frames_scored": 30}' > "$FABLE2_FLASH_DETECT/flash.json"
echo '{"sweep": {"frame_start": 2, "frame_end": 28}, "gameplay_s": 1.0}' > "$FABLE2_BENCHMARK"
[ "$mode" = after ] && crash
exit 0
'''


def main():
    if shutil.which('xcrun') is None or subprocess.run(['xcrun', '--find', 'swiftc'], capture_output=True).returncode:
        print('SKIP: no swiftc')
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        binary = tmp / 'FableIIBenchmark'
        subprocess.run(['xcrun', 'swiftc', '-O', '-swift-version', '5', '-parse-as-library', '-target',
                        'arm64-apple-macos15.0', '-o', str(binary), str(SOURCE)], check=True)
        app = tmp / 'Fake.app/Contents/MacOS'
        app.mkdir(parents=True)
        (app / 'FableIILauncher').write_text(FAKE_GAME)
        (app / 'FableIILauncher').chmod(0o755)
        (tmp / 'Fake.app/Contents/Info.plist').write_text(
            '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>'
            '<key>CFBundleIdentifier</key><string>org.fable2recomp.test.fake</string></dict></plist>')
        saves = tmp / 'saves'
        saves.mkdir()
        (saves / 'x').write_text('x')
        (tmp / 'data').mkdir()
        env = dict(os.environ, FAKE_PLAN='adapt-r1:after,adapt-snap-r1:before-once,noadapt-r1:always',
                   FABLE2_CRASH_REPORT_WAIT_S='0')
        p = subprocess.run([str(binary), '--flash', '--headless', '--reps', '1', '--arms', 'adapt,adapt-snap,noadapt,noadapt-snap',
                            '--app', str(tmp / 'Fake.app'), '--game-data', str(tmp / 'data'), '--save-dir', str(saves),
                            '--state', str(tmp / 'state'), '--out', str(tmp / 'out')],
                           env=env, capture_output=True, text=True, timeout=300)
        assert p.returncode == 0, p.stderr[-3000:]
        folder = Path(p.stdout.strip().splitlines()[-1])
        report = json.loads((folder / 'report.json').read_text())
        runs = {r['arm']: r for r in report['arms']}
        assert set(runs) == {'adapt-r1', 'adapt-snap-r1', 'noadapt-r1', 'noadapt-snap-r1'}, runs.keys()
        a = runs['adapt-r1']
        assert a['has_data'] and a['abnormal_exit'] and a['exit_signal'] == 11, a
        assert a['outcome'].startswith('data kept'), a['outcome']
        assert 'first_attempt' not in a
        b = runs['adapt-snap-r1']
        assert b['has_data'] and not b['abnormal_exit'] and b['outcome'] == 'ok on the retry', b
        assert b['first_attempt']['exit_signal'] == 11, b['first_attempt']
        assert (folder / 'raw/adapt-snap-r1/attempt1/fable_2.log').read_text().startswith('fake game adapt-snap-r1 attempt 1')
        c = runs['noadapt-r1']
        assert c['failed'] and not c['has_data'] and c['outcome'].startswith('FAILED'), c
        assert c['first_attempt']['exit_signal'] == 11
        assert (folder / 'raw/noadapt-r1/fable_2.log').read_text().startswith('fake game noadapt-r1 attempt 2')
        assert (folder / 'raw/noadapt-r1/attempt1/fable_2.log').is_file()
        d = runs['noadapt-snap-r1']
        assert d['outcome'] == 'ok' and d['has_data'], d
        summary = {s['arm']: s for s in report['summary']}
        assert summary['noadapt']['runs'] == 0 and summary['noadapt']['failed_runs'] == 1, summary['noadapt']
        assert summary['adapt']['flash_per_run'] == 3, summary['adapt']
        text = (folder / 'report.txt').read_text()
        assert 'Run problems' in text and 'adapt-r1: data kept' in text and 'noadapt-r1: FAILED' in text, text
        assert 'adapt-snap-r1: ok on the retry' in text, text
        print('benchmark failure policy: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
