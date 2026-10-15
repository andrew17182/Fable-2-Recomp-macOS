#!/usr/bin/python3
"""AIR helpers: extract <in.metallib> -> .bc, dis -> .ll (metal-opt -S), opt -O2, build .ll/.bc -> metallib."""
import struct, subprocess, sys, pathlib
def _toolchain():
    out = subprocess.run(['/usr/bin/xcrun', '--find', 'metal-opt'], capture_output=True, text=True).stdout.strip()
    return pathlib.Path(out).parent
TC = _toolchain()
def extract(lib, out):
    b = open(lib, 'rb').read(); i = b.find(bytes.fromhex('dec0170b'))
    off, size = struct.unpack('<II', b[i + 8:i + 16])
    open(out, 'wb').write(b[i:i + off + size])
def run(*a): subprocess.run([str(x) for x in a], check=True)
cmd = sys.argv[1]
if cmd == 'extract': extract(sys.argv[2], sys.argv[3])
elif cmd == 'dis': run(TC / 'metal-opt', '-S', sys.argv[2], '-o', sys.argv[3])
elif cmd == 'opt': run(TC / 'metal-opt', '-O2', sys.argv[2], '-o', sys.argv[3])
elif cmd == 'optS': run(TC / 'metal-opt', '-O2', '-S', sys.argv[2], '-o', sys.argv[3])
elif cmd == 'lib': run(TC / 'metallib', sys.argv[2], '-o', sys.argv[3])
elif cmd == 'as': run(TC / 'metal-as', sys.argv[2], '-o', sys.argv[3])
