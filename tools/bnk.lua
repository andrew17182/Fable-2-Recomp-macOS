#!/usr/bin/env python3
"""Fable 2 .bnk (script bank) reader / single-entry rewriter.

Format (big-endian), discovered from the file layout + the modding
community's CompileCompressReplace.py:

  [0x00:0x04]  baseOffset      (0x00008000; file data region starts here)
  [0x04:0x08]  00 00 00 03     (version / tag)
  [0x08]       01              (flag; 00 in levels/streaming/art banks)
  [0x09:0x0D]  tocZSize        (compressed TOC length, zlib, NO adler32 tail)
  [0x0D:0x11]  tocSize         (uncompressed TOC length)
  [0x11 .. ]   zlib(TOC)       (stream may lack the trailing adler32)

TOC (after decompression):
  [0:4]        entryCount
  per entry:
    [0:4]      nameLen          (includes the trailing NUL)
    [..]       name (NUL-terminated, backslash paths)
    Then, relative to the end of the name:
    [+0]       fileOffset       (relative to baseOffset)
    [+4]       uncompressedSize (sum of all chunk tails)
    [+8]       compressedSize   (total across all chunks)
    [+12]      flag             (0)
    [+13:15]   pad              (0 0)
    [+15]      chunkCount       (f2: number of 32KB compressed chunks)
    [+16..]    tail per chunk   (uncompressed size of each chunk;
                                 chunkCount x 4 bytes)

  Per-entry stride after the name = 16 + 4*chunkCount.

File payloads: the compressedSize bytes at baseOffset + fileOffset are
split into chunks of <= 0x8000 (32768) compressed bytes each. Each chunk
is an independent zlib stream that decompresses to that chunk's tail
size. Concatenating the chunk outputs gives the full file. Small files
(f2==1) are a single zlib stream, which is why naive single-stream
extractors work for most entries.

NOTE: levels/streaming/skeletalmorphs banks use a different (compact)
TOC meta and do not contain Lua; only the script banks are parsed here.

Usage:
  bnk.lua list <bnk>
  bnk.lua extract <bnk> <name-substr> <outdir>
  bnk.lua replace <bnk> <name-substr> <payload-file>
      - payload is written COMPRESSED at the entry's existing offset; it
        MUST be <= the entry's current compressed size (padding zeros).
      - for multi-chunk entries the new payload is written as one stream
        in chunk 0 and the remaining chunks are filled with empty
        placeholder streams; the tail sizes are updated accordingly.
"""
import struct
import sys
import zlib

CHUNK = 0x8000  # max compressed bytes per chunk


def _decompress_lenient(raw: bytes) -> bytes:
    """zlib that tolerates a missing trailing adler32."""
    d = zlib.decompressobj()
    return d.decompress(raw) + d.flush()


def read(bnk: bytes):
    base = struct.unpack(">I", bnk[0:4])[0]
    toc_z = struct.unpack(">I", bnk[0x09:0x0D])[0]
    toc_u = struct.unpack(">I", bnk[0x0D:0x11])[0]
    toc = _decompress_lenient(bnk[0x11:0x11 + toc_z])
    count = struct.unpack(">I", toc[0:4])[0]
    entries = []
    off = 4
    for _ in range(count):
        nlen = struct.unpack(">I", toc[off:off + 4])[0]
        name = toc[off + 4:off + 4 + nlen].split(b"\x00")[0].decode()
        meta = off + 4 + nlen
        o, u, c = struct.unpack(">III", toc[meta:meta + 12])
        chunks = toc[meta + 15]
        tails = [struct.unpack(">I", toc[meta + 16 + j * 4: meta + 20 + j * 4])[0]
                 for j in range(chunks)]
        entries.append(
            {
                "name": name,
                "offset": o,
                "uncomp": u,
                "comp": c,
                "chunks": chunks,
                "tails": tails,
                "toc_off": off,  # index of this entry's nameLen in the TOC
            }
        )
        off += 4 + nlen + 16 + 4 * chunks
    return base, toc_z, toc_u, toc, entries


def payload(bnk: bytes, base: int, e) -> bytes:
    out = b""
    pos = base + e["offset"]
    for j in range(e["chunks"]):
        size = min(CHUNK, e["comp"] - j * CHUNK)
        out += _decompress_lenient(bnk[pos:pos + size])
        pos += size
    return out


def list_entries(bnk: bytes):
    base, toc_z, toc_u, toc, entries = read(bnk)
    print(
        f"base={base:#x} tocZ={toc_z:#x} tocU={toc_u:#x} entries={len(entries)}"
    )
    for e in entries:
        print(
            f"{e['name']}  off={e['offset']:#08x} u={e['uncomp']:#07x} "
            f"c={e['comp']:#07x} chunks={e['chunks']}"
        )


def extract(bnk: bytes, needle: str, outdir: str):
    import os

    base, *_r, entries = read(bnk)
    os.makedirs(outdir, exist_ok=True)
    for e in entries:
        if needle in e["name"]:
            data = payload(bnk, base, e)
            fn = e["name"].replace("\\", "_")
            path = os.path.join(outdir, fn)
            with open(path, "wb") as f:
                f.write(data)
            print(f"wrote {path} ({len(data)} bytes) <- {e['name']}")


def _empty_stream():
    """smallest valid zlib stream (compresses empty data)"""
    return zlib.compress(b"")


def replace(bnk: bytes, needle: str, payload_file: str) -> bytes:
    base, toc_z, toc_u, toc, entries = read(bnk)
    matches = [e for e in entries if needle in e["name"]]
    if len(matches) != 1:
        raise SystemExit(
            f"expected exactly 1 match for {needle!r}, got {len(matches)}: "
            + ", ".join(m["name"] for m in matches)
        )
    e = matches[0]
    new_raw = open(payload_file, "rb").read()
    if len(new_raw) > e["uncomp"]:
        raise SystemExit(
            f"new payload {len(new_raw)} > entry uncompressed size {e['uncomp']}"
        )
    comp = zlib.compress(new_raw, 9)
    if len(comp) > min(CHUNK, e["comp"]):
        raise SystemExit(
            f"compressed size {len(comp)} > chunk-0 capacity "
            f"{min(CHUNK, e['comp'])}; shorten the payload"
        )

    out = bytearray(bnk)
    # 1) write the new stream in chunk 0, zero the rest of the entry
    start = base + e["offset"]
    out[start:start + len(comp)] = comp
    out[start + len(comp): start + e["comp"]] = b"\x00" * (e["comp"] - len(comp))
    # 2) multi-chunk entries: fill remaining chunks with empty streams
    for j in range(1, e["chunks"]):
        pos = start + j * CHUNK
        if pos >= start + e["comp"]:
            break
        stub = _empty_stream()
        out[pos:pos + len(stub)] = stub
    new_u = len(new_raw)
    new_c = len(comp)
    # 3) patch the TOC entry sizes + tails
    toc = bytearray(toc)
    toc_off = e["toc_off"]
    nlen = struct.unpack(">I", toc[toc_off:toc_off + 4])[0]
    m2 = toc_off + 4 + nlen
    struct.pack_into(">I", toc, m2 + 4, new_u)   # uncompressedSize
    struct.pack_into(">I", toc, m2 + 8, new_c)   # compressedSize
    struct.pack_into(">I", toc, m2 + 16, new_u)  # tail0
    for j in range(1, e["chunks"]):
        struct.pack_into(">I", toc, m2 + 16 + j * 4, 0)  # other tails empty

    # 4) recompress the TOC and rewrite the header
    toc_new = zlib.compress(bytes(toc), 9)
    struct.pack_into(">I", out, 0x09, len(toc_new))
    struct.pack_into(">I", out, 0x0D, len(toc))
    # clear the old TOC region then write the new one
    out[0x11:0x11 + toc_z] = b"\x00" * toc_z
    out[0x11:0x11 + len(toc_new)] = toc_new
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd = sys.argv[1]
    bnk = open(sys.argv[2], "rb").read()
    if cmd == "list":
        list_entries(bnk)
    elif cmd == "extract":
        extract(bnk, sys.argv[3], sys.argv[4])
    elif cmd == "replace":
        data = replace(bnk, sys.argv[3], sys.argv[4])
        with open(sys.argv[2], "wb") as f:
            f.write(data)
        print(f"wrote {sys.argv[2]} ({len(data)} bytes)")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
