# Moved from out/validation/cr-fixes-20261007/threadcpu.py (telemetry phase 1, tools/f2diag).
# threadcpu.py: per-thread CPU (user+sys ns) and names of a same-user process via libproc
# proc_pidinfo(PROC_PIDLISTTHREADS / PROC_PIDTHREADINFO). No attach, no task port: the target is
# not suspended or sampled, so it does not distort timing (unlike /usr/bin/sample).
# CLI: python3 threadcpu.py <pid>  -> prints the current table.
import ctypes, sys, collections
_lib = ctypes.CDLL('/usr/lib/libproc.dylib')
_lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
class ThreadInfo(ctypes.Structure):
    _fields_ = [('user', ctypes.c_uint64), ('system', ctypes.c_uint64), ('cpu_usage', ctypes.c_int32),
                ('policy', ctypes.c_int32), ('run_state', ctypes.c_int32), ('flags', ctypes.c_int32),
                ('sleep_time', ctypes.c_int32), ('curpri', ctypes.c_int32), ('priority', ctypes.c_int32),
                ('maxpriority', ctypes.c_int32), ('name', ctypes.c_char * 64)]
def snapshot(pid):
    """{thread_handle: (name, cpu_seconds)}"""
    buf = (ctypes.c_uint64 * 4096)()
    n = _lib.proc_pidinfo(pid, 6, 0, buf, ctypes.sizeof(buf))
    out = {}
    for h in buf[:max(0, n) // 8]:
        ti = ThreadInfo()
        if _lib.proc_pidinfo(pid, 5, h, ctypes.byref(ti), ctypes.sizeof(ti)) == ctypes.sizeof(ti):
            out[h] = (ti.name.decode(errors='replace') or '?', (ti.user + ti.system) / 1e9)
    return out
def delta(a, b):
    """Per-name CPU seconds between two snapshots (threads new in b count from 0)."""
    d = collections.defaultdict(float)
    for h, (name, cpu) in b.items():
        d[name] += cpu - (a[h][1] if h in a else 0.0)
    return dict(sorted(d.items(), key=lambda x: -x[1]))
if __name__ == '__main__':
    for h, (n, c) in sorted(snapshot(int(sys.argv[1])).items(), key=lambda x: -x[1][1]): print(f'{c:9.3f} {n}')
