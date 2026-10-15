#!/usr/bin/env python3
"""
trim_exports.py -- shrink a Windows PE's export table to a curated set.

Post-processes an already-built DLL (no recompile) so its export table lists
only the symbols named in a .def, instead of the ~10k symbols that
WINDOWS_EXPORT_ALL_SYMBOLS leaks (std/RTTI/operator/unused-__imp_ bloat). The
exported code is untouched -- only the export *directory* is rewritten -- so
consumers that import a kept symbol still resolve, and the DLL looks like a
normal product library (a far smaller, sane export count) rather than an
abnormal one.

This is one of the profile features that feeds BitDefender's generic "Ulise"
ML heuristic on unsigned novel PE files.

Usage:
    python trim_exports.py <dll> <def-file> [--inplace]

The .def is read for its symbol names (the text after '.' on each EXPORTS line,
or a bare name). Missing symbols in the def are skipped with a warning.
Idempotent: re-running with the same def keeps the same reduced table.
"""
import argparse
import os
import struct
import sys


def e_lfanew(buf):
    return struct.unpack_from("<I", buf, 0x3C)[0]


def read_sections(buf, pe_off):
    num_sections = struct.unpack_from("<H", buf, pe_off + 6)[0]
    opt_off = pe_off + 24
    opt_size = struct.unpack_from("<H", buf, pe_off + 20)[0]
    magic = struct.unpack_from("<H", buf, opt_off)[0]
    is64 = (magic == 0x20B)
    dd_off = opt_off + (112 if is64 else 104)
    section_off = opt_off + opt_size
    secs = []
    for i in range(num_sections):
        o = section_off + i * 40
        vsize = struct.unpack_from("<I", buf, o + 8)[0]
        vaddr = struct.unpack_from("<I", buf, o + 12)[0]
        rsize = struct.unpack_from("<I", buf, o + 16)[0]
        rptr = struct.unpack_from("<I", buf, o + 20)[0]
        secs.append((vaddr, vsize, rptr, rsize))
    return dd_off, secs


def rva_to_offset(buf, secs, rva):
    for vaddr, vsize, rptr, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return rptr + (rva - vaddr)
    return None


def read_def_names(def_path):
    names = set()
    for line in open(def_path, "r", encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("LIBRARY") or line.startswith("EXPORTS"):
            continue
        if line.startswith("."):
            line = line[1:]
        if line:
            names.add(line)
    return names


def trim(dll_path, def_path):
    buf = bytearray(open(dll_path, "rb").read())
    pe_off = e_lfanew(buf)
    if buf[pe_off:pe_off + 4] != b"PE\x00\x00":
        print(f"  {os.path.basename(dll_path)}: skip (not PE)", file=sys.stderr)
        return
    dd_off, secs = read_sections(buf, pe_off)
    exp_rva, _ = struct.unpack_from("<II", buf, dd_off + 0)
    if not exp_rva:
        print(f"  {os.path.basename(dll_path)}: no export table", file=sys.stderr)
        return
    eo = rva_to_offset(buf, secs, exp_rva)
    # IMAGE_EXPORT_DIRECTORY: Characteristic(0) TimeDateStamp(4)
    # MajorVersion(8) MinorVersion(10) Name(12) Base(16) NumberOfFunctions(20)
    # NumberOfNames(24) AddressOfFunctions(28) AddressOfNames(32)
    # AddressOfNameOrdinals(36)
    base = struct.unpack_from("<I", buf, eo + 16)[0]
    nf = struct.unpack_from("<I", buf, eo + 20)[0]
    nn = struct.unpack_from("<I", buf, eo + 24)[0]
    func_rva = struct.unpack_from("<I", buf, eo + 28)[0]
    name_rva = struct.unpack_from("<I", buf, eo + 32)[0]
    ord_rva = struct.unpack_from("<I", buf, eo + 36)[0]

    f_off = rva_to_offset(buf, secs, func_rva)
    n_off = rva_to_offset(buf, secs, name_rva)
    o_off = rva_to_offset(buf, secs, ord_rva)
    if None in (f_off, n_off, o_off):
        print(f"  {os.path.basename(dll_path)}: skip (export tables unresolved)",
              file=sys.stderr)
        return

    func = struct.unpack_from(f"<{nf}I", buf, f_off)
    names_rva = struct.unpack_from(f"<{nn}I", buf, n_off)
    ords = struct.unpack_from(f"<{nn}H", buf, o_off)

    def name_at(rva):
        off = rva_to_offset(buf, secs, rva)
        j = off
        b = buf
        while b[j] != 0:
            j += 1
        return bytes(b[off:j]).decode("utf-8", "replace")

    all_names = {i: name_at(r) for i, r in enumerate(names_rva)}
    want = read_def_names(def_path)

    # Build the kept set: name indices that are in `want`
    kept = [i for i in range(nn) if all_names.get(i) in want]
    missing = want - set(all_names.values())
    if missing:
        print(f"  {os.path.basename(dll_path)}: {len(missing)} def symbols not exported (skipped)",
              file=sys.stderr)
    if not kept:
        print(f"  {os.path.basename(dll_path)}: skip (nothing to keep)", file=sys.stderr)
        return

    kept_sorted = kept  # keep original name-table order (stable)
    new_nf = len(kept_sorted)

    new_func = [func[ords[i]] for i in kept_sorted]
    new_names = [names_rva[i] for i in kept_sorted]
    new_ords = list(range(new_nf))  # name i -> func i

    # Overwrite tables in place (subset fits in the original footprint)
    struct.pack_into(f"<{new_nf}I", buf, f_off, *new_func)
    struct.pack_into(f"<{new_nf}I", buf, n_off, *new_names)
    struct.pack_into(f"<{new_nf}H", buf, o_off, *new_ords)
    # Update counts
    struct.pack_into("<I", buf, eo + 20, new_nf)  # NumberOfFunctions
    struct.pack_into("<I", buf, eo + 24, new_nf)  # NumberOfNames

    open(dll_path, "wb").write(bytes(buf))
    print(f"  {os.path.basename(dll_path)}: exports {nf} -> {new_nf}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("def_file")
    ap.add_argument("--inplace", action="store_true")
    args = ap.parse_args()
    if not os.path.exists(args.dll):
        return
    trim(args.dll, args.def_file)


if __name__ == "__main__":
    main()
