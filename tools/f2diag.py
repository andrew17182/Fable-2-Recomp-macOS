#!/usr/bin/env python3
"""f2diag: flight-recorder reader and test harness (telemetry phase 1). See tools/f2diag/__main__.py."""
import signal
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from f2diag.__main__ import main  # noqa: E402

signal.signal(signal.SIGPIPE, signal.SIG_DFL)  # quiet when piped into head

if __name__ == "__main__":
    sys.exit(main())
