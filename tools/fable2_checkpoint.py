#!/usr/bin/env python3
"""Request an opt-in same-process experimental gameplay checkpoint."""
import argparse
import json
from pathlib import Path
import time


def checkpoint(session, operation, timeout=20):
    if operation not in ('save', 'load'):
        raise ValueError('Checkpoint operation must be save or load')
    session = Path(session)
    metadata = json.loads((session / 'session.json').read_text())
    if not metadata.get('checkpoint'):
        raise ValueError('Launch this session with --checkpoint first')
    folder = session / 'test-state/control'
    request = folder / 'checkpoint-request.txt'
    status = folder / 'checkpoint-status.txt'
    if request.exists():
        raise RuntimeError('A checkpoint request is already pending')
    if status.exists():
        if status.read_text().startswith('busy '):
            raise RuntimeError('A native checkpoint operation is still running')
        status.unlink()
    temporary = request.with_suffix('.tmp')
    temporary.write_text(operation + '\n')
    temporary.replace(request)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if status.exists():
            response = status.read_text().strip()
            if response.startswith('ok '):
                return response
            if response.startswith('error '):
                raise RuntimeError(response[6:])
        time.sleep(.05)
    # Cancel an unconsumed request. A consumed operation cannot be forcibly
    # unwound by the client: the native RAII guards own worker resumption.
    request.unlink(missing_ok=True)
    raise TimeoutError('Checkpoint did not finish; inspect checkpoint-status.txt and game log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('save', 'load'))
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    print(checkpoint(args.session, args.operation))


if __name__ == '__main__':
    main()
