#!/usr/bin/env python3
"""
add_version_resource.py -- add a VS_VERSION_INFO resource to an existing PE in
place (no recompile), preserving any existing resources (e.g. the manifest).

The prebuilt SDK DLLs ship with only a manifest in .rsrc and no version info,
which reads as "not a real product" to AV generic heuristics on a large unsigned
binary. This grafts a well-formed VS_VERSION_INFO (company / product / version)
into the .rsrc section. The whole resource tree is rebuilt (root -> type ->
name -> language -> data) with every existing leaf copied verbatim (e.g.
RT_MANIFEST) plus the new RT_VERSION leaf; the .rsrc section is grown and any
trailing sections are shifted in the raw file.

Usage:
    python add_version_resource.py <pe> --company "..." --product "..." \
        --description "..." --filename name.dll --version 0.10.0.0

Idempotent: if an RT_VERSION resource already exists it is left untouched.
"""
import argparse
import os
import struct

RT_VERSION = 16
ALIGN = 4  # resource tree node alignment


def e_lfanew(buf):
    return struct.unpack_from("<I", buf, 0x3C)[0]


def read_sections(buf, pe_off):
    ns = struct.unpack_from("<H", buf, pe_off + 6)[0]
    opt = pe_off + 24
    is64 = struct.unpack_from("<H", buf, opt)[0] == 0x20B
    dd = opt + (112 if is64 else 104)
    file_align = struct.unpack_from("<I", buf, opt + 32)[0] or 0x200
    soff = opt + struct.unpack_from("<H", buf, pe_off + 20)[0]
    secs = []
    for i in range(ns):
        o = soff + i * 40
        secs.append({
            "name": bytes(buf[o:o + 8]).rstrip(b"\x00"),
            "vsize": struct.unpack_from("<I", buf, o + 8)[0],
            "vaddr": struct.unpack_from("<I", buf, o + 12)[0],
            "rsize": struct.unpack_from("<I", buf, o + 16)[0],
            "rptr": struct.unpack_from("<I", buf, o + 20)[0],
            "off": o,
        })
    return dd, secs, file_align


def _align(n, a):
    return (n + a - 1) // a * a


def _pad(b, a=ALIGN):
    return b + b"\x00" * ((a - len(b) % a) % a)


def _text_value(key, value):
    keyb = _pad((key + "\x00").encode("utf-16-le"))
    valb = (value + "\x00").encode("utf-16-le")
    body = keyb + _pad(valb)
    return struct.pack("<HHH", 6 + len(body), len(valb), 1) + body


def _binary_value(key, valbytes):
    keyb = _pad((key + "\x00").encode("utf-16-le"))
    body = keyb + _pad(valbytes)
    return struct.pack("<HHH", 6 + len(body), len(valbytes), 0) + body


def _structure(key, children):
    keyb = _pad((key + "\x00").encode("utf-16-le"))
    body = keyb + b"".join(children)
    return struct.pack("<HHH", 6 + len(body), 0, 1) + body


def build_vs_version_info(f):
    ver = f.get("version", "0.0.0.0")
    strings = _structure("StringFileInfo", [_structure("040904B0", [
        _text_value("CompanyName", f.get("company", "Unknown")),
        _text_value("FileDescription", f.get("description", "")),
        _text_value("FileVersion", ver),
        _text_value("InternalName", f.get("internal", f.get("filename", "file"))),
        _text_value("LegalCopyright", f.get("copyright", "")),
        _text_value("OriginalFilename", f.get("filename", "file")),
        _text_value("ProductName", f.get("product", "Product")),
        _text_value("ProductVersion", ver),
    ])])
    var = _structure("VarFileInfo", [_binary_value("Translation", struct.pack("<HH", 0x04B0, 0x0409))])
    return _structure("VS_VERSION_INFO", [strings, var])


def _r2o(sec, rva):
    return sec["rptr"] + (rva - sec["vaddr"])


def _parse_leaves(buf, res_sec):
    """Return [(type, name, lang, data_bytes)] for the resource section."""
    base = res_sec["rptr"]
    leaves = []

    def entries(off):
        nn = struct.unpack_from("<H", buf, off + 12)[0]   # NumberOfNamedEntries
        ni = struct.unpack_from("<H", buf, off + 14)[0]   # NumberOfIdEntries
        return [(struct.unpack_from("<I", buf, off + 16 + i * 8)[0],
                 struct.unpack_from("<I", buf, off + 20 + i * 8)[0])
                for i in range(nn + ni)]

    for tid, t_off in entries(base):
        if tid & 0x80000000:
            continue
        for nid, n_off in entries(base + (t_off & 0x7FFFFFFF)):
            for lid, d_off in entries(base + (n_off & 0x7FFFFFFF)):
                de = base + (d_off & 0x7FFFFFFF)
                rva = struct.unpack_from("<I", buf, de)[0]
                size = struct.unpack_from("<I", buf, de + 4)[0]
                doff = _r2o(res_sec, rva)
                leaves.append((tid, nid, lid, bytes(buf[doff:doff + size])))
    return leaves


def _build_rsrc(leaves):
    """leaves: [(type, name, lang, data_bytes)].
    Returns (content_bytes, {leaf_key: {"entry": off, "payload": off}})."""
    types = {}
    for t, n, l, data in leaves:
        types.setdefault(t, {}).setdefault(n, []).append((l, data))
    n_types = len(types)
    type_off, name_off = {}, {}
    cur = 16 + n_types * 8
    for t in types:
        type_off[t] = cur
        cur += 16 + len(types[t]) * 8
    for t, names in types.items():
        for n in names:
            name_off[(t, n)] = cur
            cur += 16 + len(names[n]) * 8
    data_entry_off = {}
    for t, names in types.items():
        for n, langs in names.items():
            for l, _ in langs:
                data_entry_off[(t, n, l)] = cur
                cur += 16
    tree_size = cur

    out = bytearray()
    out += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, n_types)
    for t in types:
        out += struct.pack("<II", t, 0x80000000 | type_off[t])
    for t in types:
        names = types[t]
        out += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(names))
        for n in names:
            out += struct.pack("<II", n, 0x80000000 | name_off[(t, n)])
    for (t, n) in name_off:
        langs = types[t][n]
        out += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(langs))
        for l, _ in langs:
            out += struct.pack("<II", l, data_entry_off[(t, n, l)])
    info = {}
    for (t, n) in name_off:
        for l, d in types[t][n]:
            info[(t, n, l)] = {"entry": data_entry_off[(t, n, l)]}
            out += struct.pack("<IIII", 0, len(d), 0, 0)  # RVA patched later
    assert len(out) == tree_size, (len(out), tree_size)
    for (t, n) in name_off:
        for l, d in types[t][n]:
            info[(t, n, l)]["payload"] = len(out)
            out.extend(d)
    return bytes(out), info


def add_version_resource(dll, fields, log=lambda *a: None):
    buf = bytearray(open(dll, "rb").read())
    pe = e_lfanew(buf)
    dd, secs, file_align = read_sections(buf, pe)
    res_rva, _ = struct.unpack_from("<II", buf, dd + 16)  # data dir index 2 = resources
    res_sec = None
    for s in secs:
        if s["vaddr"] <= res_rva < s["vaddr"] + max(s["vsize"], s["rsize"]):
            res_sec = s
            break
    if res_sec is None:
        log("  no resource section; skip version add")
        return False
    leaves = _parse_leaves(buf, res_sec)
    if any(t == RT_VERSION for t, *_ in leaves):
        log(f"  {res_sec['name'].decode()}: already has RT_VERSION; skip")
        return False

    version_blob = build_vs_version_info(fields)
    leaves.append((RT_VERSION, 1, 0x0409, version_blob))
    content, info = _build_rsrc(leaves)

    out = bytearray(content)
    for (t, n, l), meta in info.items():
        struct.pack_into("<I", out, meta["entry"], res_sec["vaddr"] + meta["payload"])
    new_content = bytes(out)

    # guard: grown .rsrc must not overlap the next section's vaddr
    idx = secs.index(res_sec)
    if idx + 1 < len(secs) and res_sec["vaddr"] + len(new_content) > secs[idx + 1]["vaddr"]:
        log("  .rsrc too small to grow in place; skip")
        return False

    new_rsize = max(_align(len(new_content), file_align), res_sec["rsize"])
    first_rptr = min(s["rptr"] for s in secs)
    header = bytes(buf[:first_rptr])
    body = bytearray()
    body_off = {}
    running = 0
    for s in secs:
        pad = _align(running, file_align) - running
        body.extend(b"\x00" * pad)
        running += pad
        rsize = new_rsize if s is res_sec else s["rsize"]
        body_off[id(s)] = running
        if s is res_sec:
            body.extend(new_content)
            body.extend(b"\x00" * (rsize - len(new_content)))
        else:
            body.extend(bytes(buf[s["rptr"]:s["rptr"] + s["rsize"]]))
        running += rsize
    new_buf = bytearray(header + bytes(body))

    for s in secs:
        rsize = new_rsize if s is res_sec else s["rsize"]
        struct.pack_into("<I", new_buf, s["off"] + 20, first_rptr + body_off[id(s)])
        if s is res_sec:
            struct.pack_into("<I", new_buf, s["off"] + 8, len(new_content))
            struct.pack_into("<I", new_buf, s["off"] + 16, new_rsize)
    struct.pack_into("<II", new_buf, dd + 16, res_sec["vaddr"], len(new_content))
    open(dll, "wb").write(new_buf)
    log(f"  {res_sec['name'].decode()}: added RT_VERSION ({len(leaves)} leaves, "
        f".rsrc now {len(new_content)} bytes)")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pe")
    ap.add_argument("--company", default="")
    ap.add_argument("--product", default="")
    ap.add_argument("--description", default="")
    ap.add_argument("--internal", default="")
    ap.add_argument("--copyright", default="")
    ap.add_argument("--version", default="0.10.0.0")
    a = ap.parse_args()
    filename = os.path.basename(a.pe)
    fields = dict(company=a.company, product=a.product, description=a.description,
                  filename=filename, internal=a.internal or os.path.splitext(filename)[0],
                  copyright=a.copyright, version=a.version)
    if add_version_resource(a.pe, fields, log=print):
        print("  done")


if __name__ == "__main__":
    main()
