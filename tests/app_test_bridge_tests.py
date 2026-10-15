import json
from pathlib import Path
import subprocess
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
class MCPTests(unittest.TestCase):
 def test_protocol_and_error_recovery(self):
  requests = [json.dumps({'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2024-11-05'}}),
    json.dumps({'jsonrpc':'2.0','method':'notifications/initialized'}),
    'invalid-json', json.dumps({'jsonrpc':'2.0','id':2,'method':'tools/list'}),
    json.dumps({'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'press','arguments':{'session':'missing','key':'INVALID'}}})]
  result = subprocess.run([sys.executable,str(ROOT/'tools/fable2_test_mcp.py')],input='\n'.join(requests)+'\n',text=True,capture_output=True,check=True)
  replies = [json.loads(line) for line in result.stdout.splitlines()]
  self.assertEqual(replies[0]['result']['protocolVersion'],'2024-11-05')
  self.assertIn('error',replies[1])
  self.assertEqual({t['name'] for t in replies[2]['result']['tools']},{'launch','capture','press','close','summary','ping','telemetry','stop_recording','replay','cancel_replay','build','build_status'})
  self.assertTrue(replies[3]['result']['isError'])
if __name__ == '__main__': unittest.main()
