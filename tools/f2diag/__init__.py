"""f2diag: reader and analyses for the Fable II flight recorder (telemetry phase 1).

CLI: python3 tools/f2diag.py <command> ... (see `python3 tools/f2diag.py -h`).
Library: from f2diag import Session, f2t, timeline, images, run, threadcpu
"""
from .session import Session  # noqa: F401
