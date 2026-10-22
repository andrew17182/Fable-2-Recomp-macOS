#!/usr/bin/env python3
"""Replay guest pad snapshots against observed state transitions (no images).

Recordings (docs/reference.md "Input recording"): schema 1 (host time only), schema 2 (adds guest time
g_us / gdt_us and "speed" events) and schema 3 (pad sticks from a controller are raw, before the deadzone
remap: "raw" names them, bit 0 left / bit 1 right, and "game" holds what the game read when it differs).
load_recording validates all three and accepts a crash-truncated final line. The host-clock driver below
sends "game" (else "pad"), so it replays what the game read, not a remap under the current settings.
The default replay driver is the game itself (FABLE2_REPLAY_INPUT / control/replay-input.txt), which schedules
by guest time, so it holds at any turbo speed. This module's Replay class is the old host-clock driver
(fable2_test_control.py replay --host-clock): exact only when recording and replay both run at 1x."""
import argparse
import json
from pathlib import Path
import time

NEUTRAL = [0] * 7

def clock_us():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000


def load_recording(path):
    lines = [line for line in Path(path).read_text().splitlines() if line]
    events = []
    for i, line in enumerate(lines):
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            if i == len(lines) - 1: break  # a crash can cut the last line short: the prefix stays valid
            raise ValueError('Recording line %d is not JSON' % (i + 1))
    if not events or events[0].get('state') != 'AppLaunch' or events[0].get('t_us') != 0:
        raise ValueError('Recording must start with the AppLaunch anchor')
    anchors = {}
    previous_seq, previous_time = -1, -1
    for event in events:
        if event.get('schema') not in (1, 2, 3) or event.get('type') not in ('state', 'key', 'pad', 'speed', 'recording_end'):
            raise ValueError('Unsupported recording schema/event')
        if not isinstance(event.get('seq'), int) or event['seq'] <= previous_seq:
            raise ValueError('Recording event sequence is not increasing')
        if not isinstance(event.get('t_us'), int) or event['t_us'] < previous_time:
            raise ValueError('Recording timestamps are not increasing')
        previous_seq, previous_time = event['seq'], event['t_us']
        if event['type'] == 'state' and event['state'] != 'Unknown':
            anchors[event['seq']] = event
        if event['anchor'] not in anchors:
            raise ValueError('Input refers to a missing or future anchor')
        if abs(event['dt_us'] - (event['t_us'] - anchors[event['anchor']]['t_us'])) > 1000:
            raise ValueError('Inconsistent anchor-relative timing')
        if event['schema'] >= 2 and not (isinstance(event.get('g_us'), int) and isinstance(event.get('gdt_us'), int)):
            raise ValueError('Schema %d event without guest time' % event['schema'])
        if event['type'] == 'pad':
            limits = [(0, 65535), (0, 255), (0, 255)] + [(-32768, 32767)] * 4
            for key in ('pad', 'game') if 'game' in event else ('pad',):
                pad = event.get(key)
                if not isinstance(pad, list) or len(pad) != 7 or any(
                        type(v) is not int or not lo <= v <= hi for v, (lo, hi) in zip(pad, limits)):
                    raise ValueError('Invalid controller snapshot')
            if type(event.get('raw', 0)) is not int or not 0 <= event.get('raw', 0) <= 3:
                raise ValueError('Invalid raw stick mask')
    if not any(e['type'] == 'pad' for e in events):
        raise ValueError('Recording contains no guest controller samples')
    if events[-1]['type'] != 'recording_end':
        # EOF is a stop at the last recorded timestamp. Keep this terminal event
        # in memory only: the original trace is never rewritten.
        last = events[-1]
        end = dict(schema=last['schema'], seq=last['seq'] + 1, type='recording_end',
                   t_us=last['t_us'], anchor=last['anchor'], dt_us=last['dt_us'])
        if last['schema'] >= 2: end.update(g_us=last['g_us'], gdt_us=last['gdt_us'])
        events.append(end)
    return events


def atomic_write(path, value):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(value)
    temp.replace(path)


class Replay:
    def __init__(self, recording, session, timeout=60):
        self.events = load_recording(recording)
        self.session = Path(session)
        self.folder = self.session / 'test-state/control'
        self.origin = json.loads((self.session / 'session.json').read_text())['origin_us']
        self.timeout = timeout
        self.pad = NEUTRAL.copy()
        self.live = []
        self.cursor = 0
        self.anchor_times = {0: 0}
        self.matched = []

    def status(self, state, **fields):
        atomic_write(self.folder / 'replay-status.json', json.dumps(
            dict(status=state, matched=self.matched, **fields), indent=2) + '\n')

    def publish(self, active=True):
        # Refresh the lease while waiting; a crash cannot leave a stick held.
        lease = clock_us() + 500000
        atomic_write(self.folder / 'replay-pad.txt',
                     ' '.join(map(str, [int(active), lease, *self.pad])) + '\n')

    def tick(self):
        if (self.folder / 'cancel-replay').exists():
            raise RuntimeError('Replay cancelled')
        self.publish()
        time.sleep(.005)

    def read_live(self):
        path = self.folder / 'events.jsonl'
        if not path.exists(): return []
        events = []
        for line in path.read_text().splitlines():
            try: event = json.loads(line)
            except json.JSONDecodeError: continue
            if event['type'] == 'state' and event['state'] != 'Unknown': events.append(event)
        return events

    def sync(self, anchor):
        if anchor['seq'] in self.anchor_times: return self.anchor_times[anchor['seq']]
        deadline = time.monotonic() + self.timeout
        self.status('waiting', expected=anchor['state'])
        while time.monotonic() < deadline:
            live = self.read_live()
            while self.cursor < len(live):
                event = live[self.cursor]
                self.cursor += 1
                if event['state'] == anchor['state']:
                    self.anchor_times[anchor['seq']] = event['t_us']
                    self.matched.append({'state': anchor['state'], 'recorded_seq': anchor['seq'],
                                         'live_seq': event['seq']})
                    return event['t_us']
            self.tick()
        raise TimeoutError('No matching transition: ' + anchor['state'])

    def run(self):
        try:
            self.status('running')
            anchors = {e['seq']: e for e in self.events if e['type'] == 'state' and e['state'] != 'Unknown'}
            # Match reliable transitions in recorded order, including ones with
            # no subsequent input. Unknown never resets the replay clock.
            for event in self.events:
                if event['type'] == 'state' and event['state'] != 'Unknown':
                    self.sync(event)
                elif event['type'] in ('pad', 'recording_end'):
                    target = self.sync(anchors[event['anchor']]) + event['dt_us']
                    while clock_us() - self.origin < target:
                        self.tick()
                    if event['type'] == 'pad':
                        self.pad = event.get('game', event['pad'])  # what the game read (schema 3: "pad" sticks are raw)
                        self.publish()
            self.status('complete')
        except Exception as error:
            self.status('failed', error=str(error))
        finally:
            self.pad = NEUTRAL.copy()
            # Leave a short neutral override, then automatically return control
            # to the human. Natural inputs are never permanently disabled.
            self.publish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path)
    parser.add_argument('session', type=Path)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    Replay(args.recording, args.session, args.timeout).run()


if __name__ == '__main__': main()
