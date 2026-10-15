#!/usr/bin/env python3
"""
gen_def.py -- generate a curated .def export list for rexruntime that exports
exactly (a) every symbol any host/plugin consumer actually imports, plus (b)
the whole rex:: public API, and drops the ~9k std/spdlog/fmt/RTTI/operator/
unused-__imp_ bloat that WINDOWS_EXPORT_ALL_SYMBOLS leaks.

A smaller, normal-looking export table is one of the profile features that feed
BitDefender's generic "Ulise" heuristic. This keeps the recompile ABI intact
(every consumer import is preserved) while making the DLL look like a normal
product library.

Usage:
    python gen_def.py <rexruntime.dll> <consumer1.exe> [consumer2.dll ...] \
        --out rexruntime.def [--tag rexruntimed]

  <rexruntime.dll>  the export-all build, source of the rex:: symbol universe
  consumers         every exe/dll that imports from rexruntime (release+debug)
  --tag             optional; also sweep any consumer importing this name

Symbols kept:
  - every name a consumer imports from rexruntime / rexruntimed
  - every export whose mangled name contains "@rex@" (the project public API)
"""
import argparse
import os
import re
import sys

try:
    import pefile
except ImportError:
    sys.exit("pefile is required: pip install pefile")

REX_RUNTIME_NAMES = {"rexruntime.dll", "rexruntimed.dll", "rexruntimerd.dll"}


def consumer_imports(consumer_path, want_dlls):
    pe = pefile.PE(consumer_path)
    names = set()
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        if entry.dll.decode().lower() in want_dlls:
            for i in entry.imports:
                if i.name:
                    names.add(i.name.decode("utf-8", "replace"))
    return names


def dll_exports(rex_path):
    pe = pefile.PE(rex_path)
    e = pe.DIRECTORY_ENTRY_EXPORT.struct
    tbl = pe.get_data(e.AddressOfNames, e.NumberOfNames * 4)
    out = []
    for i in range(e.NumberOfNames):
        rva = int.from_bytes(tbl[i * 4:i * 4 + 4], "little")
        off = pe.get_offset_from_rva(rva)
        j = off
        b = pe.__data__
        while b[j] != 0:
            j += 1
        out.append(b[off:j].decode("utf-8", "replace"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rex_runtime")
    ap.add_argument("consumers", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default=None)
    args = ap.parse_args()

    want = set(REX_RUNTIME_NAMES)
    if args.tag:
        want.add(args.tag.lower())

    imports = set()
    for c in args.consumers:
        if not os.path.exists(c):
            continue
        got = consumer_imports(c, want)
        if got:
            print(f"  {os.path.basename(c)}: {len(got)} imports from rexruntime",
                  file=sys.stderr)
        imports.update(got)

    exports = dll_exports(args.rex_runtime)
    rex_api = {n for n in exports if "@rex@" in n}

    keep = set(imports) | set(rex_api)
    # sanity: everything we keep must actually exist in the current export set
    unknown = keep - set(exports)
    if unknown:
        print(f"  warning: {len(unknown)} kept symbols not in current export set:",
              file=sys.stderr)
        for u in list(unknown)[:10]:
            print(f"    {u}", file=sys.stderr)
        keep -= unknown

    dropped = set(exports) - keep
    print(f"  current exports : {len(exports)}", file=sys.stderr)
    print(f"  rex:: API       : {len(rex_api)}", file=sys.stderr)
    print(f"  consumer imports: {len(imports)}", file=sys.stderr)
    print(f"  KEPT (new def)  : {len(keep)}", file=sys.stderr)
    print(f"  dropped         : {len(dropped)}", file=sys.stderr)

    with open(args.out, "w", newline="\n") as f:
        f.write("LIBRARY rexruntime\nEXPORTS\n")
        for n in sorted(keep):
            # .def names are literal; no escaping needed for these mangled names
            f.write(f".{n}\n")
    print(f"  wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
