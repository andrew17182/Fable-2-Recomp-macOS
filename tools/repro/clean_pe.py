#!/usr/bin/env python3
"""
clean_pe.py -- one-shot, self-contained post-build cleaner for the staged
release dir. For every relevant Windows binary it:

  1. TRIMS the rexruntime export table to the curated set -- the union of
     (a) every symbol any consumer actually imports from rexruntime, plus
     (b) the whole rex:: public API -- dropping the ~6k std/RTTI/operator/
     unused-__imp_ symbols that WINDOWS_EXPORT_ALL_SYMBOLS leaks. The .def is
     regenerated from the *current* consumers on every run, so it can't go
     stale as the host's imports grow.
  2. NORMALIZES the volatile PE bytes (timestamp, PDB GUID, PDB path) so an
     identical source builds to an identical hash.

Both steps reduce the "abnormal novel PE" profile that drives AV generic
heuristics (BitDefender "Ulise") on unsigned builds. See README.md.

Usage:
    python clean_pe.py <build-dir> [--stamp 0x6554B000] [--quiet]

Requires: pefile  (pip install pefile)
"""
import argparse
import glob
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import pefile
except ImportError:
    sys.exit("pefile is required: pip install pefile")

from normalize_pe import DEFAULT_STAMP, normalize as normalize_one  # reuse
from add_version_resource import add_version_resource  # reuse

REX_RUNTIME = ("rexruntime.dll", "rexruntimed.dll", "rexruntimerd.dll")
SDK_VERSION = "0.10.0.0"  # keep in sync with the prebuilt SDK version


def version_fields(dll_path):
    """VS_VERSION_INFO fields derived from a ReXGlue DLL's filename."""
    name = os.path.basename(dll_path)
    stem = os.path.splitext(name)[0]
    low = name.lower()
    if "xenos" in low:
        desc = "ReXGlue Xenos GPU Plugin"
    elif "runtime" in low:
        desc = "ReXGlue Runtime"
    else:
        desc = "ReXGlue"
    return dict(company="ReXGlue Project", product="ReXGlue", description=desc,
                filename=name, internal=stem, copyright="Copyright (C) ReXGlue Project",
                version=SDK_VERSION)


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def read_sections(buf, pe_off):
    ns = struct.unpack_from("<H", buf, pe_off + 6)[0]
    opt = pe_off + 24
    is64 = struct.unpack_from("<H", buf, opt)[0] == 0x20B
    dd = opt + (112 if is64 else 104)
    soff = opt + struct.unpack_from("<H", buf, pe_off + 20)[0]
    secs = []
    for i in range(ns):
        o = soff + i * 40
        secs.append((struct.unpack_from("<I", buf, o + 12)[0],
                     struct.unpack_from("<I", buf, o + 8)[0],
                     struct.unpack_from("<I", buf, o + 20)[0],
                     struct.unpack_from("<I", buf, o + 16)[0]))
    return dd, secs


def r2o(buf, secs, rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs):
            return rp + (rva - va)
    return None


def import_names_from(path, want_dlls):
    pe = pefile.PE(path)
    try:
        out = set()
        for e in pe.DIRECTORY_ENTRY_IMPORT:
            if e.dll.decode().lower() in want_dlls:
                for i in e.imports:
                    if i.name:
                        out.add(i.name.decode("utf-8", "replace"))
        return out
    finally:
        pe.close()


def export_names(path):
    pe = pefile.PE(path)
    try:
        e = pe.DIRECTORY_ENTRY_EXPORT.struct
        tbl = pe.get_data(e.AddressOfNames, e.NumberOfNames * 4)
        out = []
        b = pe.__data__
        for i in range(e.NumberOfNames):
            rva = int.from_bytes(tbl[i * 4:i * 4 + 4], "little")
            off = pe.get_offset_from_rva(rva)
            j = off
            while b[j] != 0:
                j += 1
            out.append(b[off:j].decode("utf-8", "replace"))
        return out
    finally:
        pe.close()


def trim_export_table(dll, keep):
    """Rewrite the export directory to list only `keep` (by name). In place."""
    buf = bytearray(open(dll, "rb").read())
    pe = struct.unpack_from("<I", buf, 0x3C)[0]
    dd, secs = read_sections(buf, pe)
    exp_rva, _ = struct.unpack_from("<II", buf, dd)
    eo = r2o(buf, secs, exp_rva)
    nf = struct.unpack_from("<I", buf, eo + 20)[0]
    nn = struct.unpack_from("<I", buf, eo + 24)[0]
    f_off = r2o(buf, secs, struct.unpack_from("<I", buf, eo + 28)[0])
    n_off = r2o(buf, secs, struct.unpack_from("<I", buf, eo + 32)[0])
    o_off = r2o(buf, secs, struct.unpack_from("<I", buf, eo + 36)[0])
    if None in (f_off, n_off, o_off):
        return False
    func = struct.unpack_from(f"<{nf}I", buf, f_off)
    names_rva = struct.unpack_from(f"<{nn}I", buf, n_off)
    ords = struct.unpack_from(f"<{nn}H", buf, o_off)

    def name_at(rva):
        off = r2o(buf, secs, rva)
        j = off
        while buf[j] != 0:
            j += 1
        return bytes(buf[off:j]).decode("utf-8", "replace")

    # keep name-table indices whose names are in `keep` (original order)
    kept = [i for i in range(nn) if name_at(names_rva[i]) in keep]
    if not kept:
        return False
    new_nf = len(kept)
    new_func = [func[ords[i]] for i in kept]
    new_names = [names_rva[i] for i in kept]
    struct.pack_into(f"<{new_nf}I", buf, f_off, *new_func)
    struct.pack_into(f"<{new_nf}I", buf, n_off, *new_names)
    struct.pack_into(f"<{new_nf}H", buf, o_off, *range(new_nf))
    struct.pack_into("<I", buf, eo + 20, new_nf)
    struct.pack_into("<I", buf, eo + 24, new_nf)
    open(dll, "wb").write(bytes(buf))
    return new_nf


def clean_dir(directory, stamp, quiet):
    dlls = sorted(glob.glob(os.path.join(directory, "*.dll")))
    exes = sorted(glob.glob(os.path.join(directory, "*.exe")))

    runtime = [d for d in dlls if os.path.basename(d).lower() in REX_RUNTIME]
    consumers = list(exes) + [d for d in dlls
                              if os.path.basename(d).lower().startswith("rexgpu-xenos")]

    report = []
    # 0. add a VS_VERSION_INFO to every ReXGlue DLL that lacks one (prebuilt SDK
    #    DLLs ship with only a manifest; a version resource makes a large
    #    unsigned DLL read as a real product rather than a dropped payload).
    for d in dlls:
        low = os.path.basename(d).lower()
        if not low.startswith("rex"):
            continue
        try:
            if add_version_resource(d, version_fields(d)):
                report.append(f"  {os.path.basename(d)}: added VS_VERSION_INFO")
        except Exception as ex:
            report.append(f"  {os.path.basename(d)}: version ERROR {ex}")

    # 1. trim each runtime dll using the current consumers' imports + rex:: API
    for rt in runtime:
        want = set(REX_RUNTIME) | {os.path.basename(c).lower() for c in consumers}
        want_names = set()
        for c in consumers:
            want_names |= import_names_from(c, want)
        try:
            rt_exports = export_names(rt)
        except Exception:
            continue
        keep = want_names | {n for n in rt_exports if "@rex@" in n}
        before = len(rt_exports)
        newcount = trim_export_table(rt, keep)
        if newcount:
            report.append(f"  {os.path.basename(rt)}: exports {before} -> {newcount}")

    # 2. normalize every exe + dll in the dir (pins ts / PDB GUID / PDB path)
    for p in sorted(set(exes) | set(dlls)):
        try:
            changed = normalize_one(p, stamp,
                                    os.path.splitext(os.path.basename(p))[0] + ".pdb",
                                    quiet=True)
            report.append(f"  {os.path.basename(p)}: "
                          + ("normalized" if changed else "already normalized"))
        except Exception as ex:
            report.append(f"  {os.path.basename(p)}: ERROR {ex}")

    if not quiet:
        for line in report:
            print(line)
        report_files = sorted(set(exes) | {d for d in dlls
                                           if os.path.basename(d).lower().startswith(("rex", "fable"))})
        print("  final hashes:")
        for p in report_files:
            print(f"    {sha256(p)[:20]}  {os.path.basename(p)}")
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--stamp", type=lambda x: int(x, 0), default=DEFAULT_STAMP)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    clean_dir(args.directory, args.stamp, args.quiet)


if __name__ == "__main__":
    main()
