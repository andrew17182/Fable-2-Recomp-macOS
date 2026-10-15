#!/usr/bin/env python3
"""Local stdio MCP bridge to Fable II's opt-in app-owned test interface."""
import base64
import importlib
import json
from pathlib import Path
import sys
import fable2_test_control as control

TOOLS = [
 ('launch', 'Launch normal app with copied saves/settings; optionally record all inputs from launch.', {'display_sync': {'type':'boolean','default':True}, 'record': {'type':'boolean','default':False}, 'save': {'type':'string','description':'optional save-library entry (out/save-library/<name>) used as the only hero save'}}, []),
 ('telemetry', 'Read observed state transitions, input counts and replay progress without images.', {'session':{'type':'string'}}, ['session']),
 ('stop_recording', 'Stop input recording, preserving launch timestamps and state-relative timing.', {'session':{'type':'string'}}, ['session']),
 ('replay', 'Launch a fresh session from a recording and replay controller snapshots synchronized to telemetry.', {'recording':{'type':'string'}, 'save': {'type':'string','description':'optional save-library entry (out/save-library/<name>) used as the only hero save'}}, ['recording']),
 ('cancel_replay', 'Cancel replay and release injected controller inputs.', {'session':{'type':'string'}}, ['session']),
 ('capture', 'View game output as a PNG with native frame/capture timestamps and client delivery metadata.', {'session':{'type':'string'}}, ['session']),
 ('press', 'Press a game key; e is A with the current keyboard map. Hold in ms.', {'session':{'type':'string'},'key':{'type':'string','enum':list(control.KEYS)},'hold':{'type':'integer','minimum':20,'maximum':10000,'default':150}}, ['session','key']),
 ('close', 'Close the owned test game normally and verify process exit.', {'session':{'type':'string'}}, ['session']),
 ('summary', 'Summarize the last N seconds of the session frame recording.', {'session':{'type':'string'},'last':{'type':'number','default':120}}, ['session']),
 ('ping', 'Check the app-owned test endpoint.', {'session':{'type':'string'}}, ['session']),
 ('build', 'Start the standard macOS Metal build (tools/build_macos_metal.py) in the background; refuses while the game is open. Poll build_status.', {}, []),
 ('build_status', 'State of the last build started with build, plus the end of its log.', {'tail':{'type':'integer','default':20,'minimum':0,'maximum':200}}, []),
]


def call(name, args):
    # Pick up edits to the controller without restarting this long-lived server.
    importlib.reload(control)
    if name == 'launch': result = control.launch(args.get('display_sync', True), record=args.get('record', False), save=args.get('save'))
    elif name == 'telemetry': result = control.telemetry(args['session'])
    elif name == 'stop_recording': result = control.stop_recording(args['session'])
    elif name == 'replay': result = control.launch(replay=args['recording'], save=args.get('save'))
    elif name == 'cancel_replay':
        (Path(args['session']) / 'test-state/control/cancel-replay').touch()
        result = 'cancellation requested'
    elif name == 'capture':
        metadata = control.capture_with_metadata(args['session'])
        path = Path(metadata['path'])
        return {'content':[{'type':'image','mimeType':'image/png','data':base64.b64encode(path.read_bytes()).decode()}, {'type':'text','text':json.dumps(metadata)}]}
    elif name == 'press': result = control.press(args['session'], args['key'], args.get('hold',150))
    elif name == 'close': result = control.close(args['session'])
    elif name == 'summary': result = control.summarize(args['session'], args.get('last',120))
    elif name == 'ping': result = control.request(args['session'], 'ping')
    elif name == 'build': result = control.build()
    elif name == 'build_status': result = control.build_status(args.get('tail', 20))
    else: raise ValueError('Unknown tool')
    return {'content':[{'type':'text','text':json.dumps(result)}]}


def handle(request):
    method = request.get('method')
    if method == 'initialize':
        return {'protocolVersion':request.get('params',{}).get('protocolVersion','2024-11-05'),
                'capabilities':{'tools':{}},'serverInfo':{'name':'fable2-app-test','version':'1.0'}}
    if method == 'ping': return {}
    if method == 'tools/list':
        return {'tools':[{'name':n,'description':d,'inputSchema':{'type':'object','properties':props,'required':req,'additionalProperties':False}} for n,d,props,req in TOOLS]}
    if method == 'tools/call':
        try:
            p = request['params']; return call(p['name'],p.get('arguments',{}))
        except Exception as error:
            return {'isError':True,'content':[{'type':'text','text':str(error)}]}
    raise ValueError('Unsupported method')


def main():
    for line in sys.stdin:
        request = None
        try:
            request = json.loads(line)
            if 'id' not in request: continue
            result = handle(request)
            response = {'jsonrpc':'2.0','id':request['id'],'result':result}
        except Exception as error:
            response = {'jsonrpc':'2.0','id':request.get('id') if isinstance(request,dict) else None,'error':{'code':-32603,'message':str(error)}}
        print(json.dumps(response),flush=True)

if __name__ == '__main__': main()
