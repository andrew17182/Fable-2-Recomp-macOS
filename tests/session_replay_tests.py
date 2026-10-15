#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import fable2_replay as replay


def event(seq,kind,t,anchor=0,dt=None,**kwargs):
    return dict(schema=1,seq=seq,type=kind,t_us=t,anchor=anchor,
                dt_us=t if dt is None else dt,**kwargs)


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.root=Path(self.tmp.name)
        self.recording=self.root/'recording.jsonl'
        self.session=self.root/'session'
        self.folder=self.session/'test-state/control'
        self.folder.mkdir(parents=True)
        (self.session/'session.json').write_text(json.dumps({'origin_us':0}))

    def tearDown(self): self.tmp.cleanup()

    def save(self,events):
        self.recording.write_text(''.join(json.dumps(e)+'\n' for e in events))

    def test_preserves_launch_inputs_and_releases_across_anchors(self):
        events=[event(0,'state',0,state='AppLaunch'),
                event(1,'key',50,key=69,down=True,repeat=False),
                event(2,'pad',100,pad=[4096,0,0,0,0,0,0]),
                event(3,'state',200,anchor=3,dt=0,state='MainMenu'),
                event(4,'pad',250,anchor=3,dt=50,pad=replay.NEUTRAL),
                event(5,'recording_end',300,anchor=3,dt=100)]
        self.save(events)
        self.assertEqual(replay.load_recording(self.recording),events)

    def test_unknown_does_not_become_sync_anchor(self):
        self.save([event(0,'state',0,state='AppLaunch'),
                   event(1,'state',200,state='Unknown'),
                   event(2,'pad',250,pad=replay.NEUTRAL),
                   event(3,'recording_end',300)])
        self.assertEqual(replay.load_recording(self.recording)[2]['anchor'],0)

    def test_eof_is_stop_without_changing_original_file(self):
        self.save([event(0,'state',0,state='AppLaunch'),
                   event(1,'pad',100,pad=[4096,0,0,0,0,0,0]),
                   event(2,'state',200,anchor=2,dt=0,state='MainMenu')])
        original = self.recording.read_bytes()
        events = replay.load_recording(self.recording)
        self.assertEqual(events[-1], event(3,'recording_end',200,anchor=2,dt=0))
        self.assertEqual(self.recording.read_bytes(),original)

    def test_eof_releases_held_controller_input(self):
        self.save([event(0,'state',0,state='AppLaunch'),
                   event(1,'pad',100,pad=[4096,0,0,32767,0,0,0])])
        worker=replay.Replay(self.recording,self.session)
        sent=[]
        with patch.object(replay,'clock_us',return_value=100), \
             patch.object(worker,'publish',side_effect=lambda:sent.append(worker.pad.copy())):
            worker.run()
        self.assertEqual(sent,[[4096,0,0,32767,0,0,0],replay.NEUTRAL])
        self.assertEqual(json.loads((self.folder/'replay-status.json').read_text())['status'],'complete')

    def test_rejects_invalid_pad_and_future_anchor(self):
        for change in ({'pad':[70000,0,0,0,0,0,0]},{'anchor':9}):
            pad=event(1,'pad',100,pad=replay.NEUTRAL);pad.update(change)
            self.save([event(0,'state',0,state='AppLaunch'),pad,event(2,'recording_end',200)])
            with self.assertRaises(ValueError):replay.load_recording(self.recording)

    def test_sync_uses_live_transition_time_and_repeated_states_in_order(self):
        self.save([event(0,'state',0,state='AppLaunch'),event(1,'pad',100,pad=replay.NEUTRAL),event(2,'recording_end',200)])
        worker=replay.Replay(self.recording,self.session)
        live=[event(0,'state',0,state='AppLaunch'),event(1,'state',400,state='MainMenu'),
              event(2,'state',900,state='HeroSelect'),event(3,'state',1800,state='MainMenu')]
        with patch.object(worker,'read_live',return_value=live):
            self.assertEqual(worker.sync(event(10,'state',200,state='MainMenu')),400)
            self.assertEqual(worker.sync(event(11,'state',500,state='HeroSelect')),900)
            self.assertEqual(worker.sync(event(12,'state',700,state='MainMenu')),1800)

    def test_delay_rebases_pad_timing(self):
        self.save([event(0,'state',0,state='AppLaunch'),
                   event(1,'state',100000,anchor=1,dt=0,state='MainMenu'),
                   event(2,'pad',150000,anchor=1,dt=50000,pad=[4096,0,0,0,0,0,0]),
                   event(3,'pad',200000,anchor=1,dt=100000,pad=replay.NEUTRAL),
                   event(4,'recording_end',210000,anchor=1,dt=110000)])
        worker=replay.Replay(self.recording,self.session)
        clock=[1000000]; sent=[]
        def tick():clock[0]+=5000
        def publish(active=True):sent.append((clock[0],worker.pad.copy()))
        def sync(anchor):return 1000000 if anchor['seq']==1 else 0
        with patch.object(worker,'sync',side_effect=sync),patch.object(worker,'tick',side_effect=tick),\
             patch.object(worker,'publish',side_effect=publish),patch.object(replay,'clock_us',side_effect=lambda:clock[0]):
            worker.run()
        self.assertIn((1050000,[4096,0,0,0,0,0,0]),sent)
        self.assertIn((1100000,replay.NEUTRAL),sent)

    def test_missing_transition_reports_failure_and_releases_input(self):
        self.save([event(0,'state',0,state='AppLaunch'),
                   event(1,'state',100,anchor=1,dt=0,state='Missing'),
                   event(2,'pad',200,anchor=1,dt=100,pad=replay.NEUTRAL),
                   event(3,'recording_end',300,anchor=1,dt=200)])
        worker=replay.Replay(self.recording,self.session,timeout=0)
        worker.run()
        status=json.loads((self.folder/'replay-status.json').read_text())
        self.assertEqual(status['status'],'failed')
        self.assertIn('Missing',status['error'])
        self.assertEqual(list(map(int,(self.folder/'replay-pad.txt').read_text().split()))[2:],replay.NEUTRAL)


if __name__=='__main__': unittest.main()
