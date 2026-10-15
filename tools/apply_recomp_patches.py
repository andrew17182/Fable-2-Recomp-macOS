#!/usr/bin/env python3
"""Apply recomp-level patches to codegen's output (generated/default/*.cpp).

Recomps run NATIVE code, so a Xenia code-region patch (a guest .text byte
change) has no runtime effect on its own. PREFERRED mechanism: the SDK's
mid-asm hooks ([[entrypoint.midasm_hook]] in fable_2_manifest.toml + a hook
function in src/fable2_hooks.cpp) - see docs/patches.md. This script is the
fallback for patches that cannot be expressed as hooks (e.g. constants
baked into memory operands). It runs as a build step between codegen and
compiling fable_2_recomp (wired in CMakeLists.txt), keyed on
codegen.build.stamp, so patches survive codegen re-runs.

Patches are applied as anchored text replacements with a unique marker
comment (idempotent: already-patched files are left alone). Missing anchors
are a hard error - a silently-skipped patch is worse than a failed build.

Set FABLE2_RECOMP_PATCHES=0 to skip all patches (used for A/B runs).
"""

import os
import re
import sys
from apply_startup_handoff import apply as apply_startup_handoff

GENERATED_DIR = sys.argv[1] if len(sys.argv) > 1 else "generated/default"

# Each patch: (name, anchor, replacement). The anchor must occur exactly once
# per generated file that contains the target function. The replacement ends
# with the patch's marker comment, which is the idempotency check.
# Patches that cannot be expressed as a [[entrypoint.midasm_hook]] (see
# docs/patches.md).
PATCHES = []

MARKER = "// [recomp-patch: "

# Guest memory barriers. ReXGlue 0.10.0 codegen emits PPC sync/lwsync as no-ops
# (it assumes x86 ordering); Apple Silicon is weakly ordered, so every barrier
# comment line gets a host fence macro (src/core/fable2_guest_memory.h, runtime
# switch FABLE2_GUEST_BARRIERS). Idempotent: lines already followed by the
# macro are left alone. The source-built SDK's codegen emits REX_GUEST_SYNC /
# REX_GUEST_LWSYNC itself (manifest memory_barriers = true; phase B3,
# patches/sdk/codegen-memory-barriers.patch); those lines count as fenced, so
# this pass inserts 0 there and stays the fallback for the prebuilt rexglue.
BARRIER_MARKER = MARKER + "guest-barrier]"
BARRIER_RE = re.compile(r"^(\t// (lwsync|sync) *)\n(?!\t(?:FABLE2|REX)_GUEST_)", re.M)
CODEGEN_FENCE_RE = re.compile(r"^\tREX_GUEST_(?:LW)?SYNC\(\);$", re.M)


def apply_guest_barriers(files):
    added = 0
    for path in files:
        with open(path, "rb") as f:
            data = f.read().decode("utf-8")

        def repl(m):
            macro = "FABLE2_GUEST_LWSYNC" if m.group(2) == "lwsync" else "FABLE2_GUEST_SYNC"
            return m.group(1) + "\n\t" + macro + "(); " + BARRIER_MARKER + "\n"

        new, n = BARRIER_RE.subn(repl, data)
        if n:
            with open(path, "wb") as f:
                f.write(new.encode("utf-8"))
            added += n
    total = codegen = 0
    for path in files:
        with open(path, "rb") as f:
            data = f.read().decode("utf-8")
        total += data.count(BARRIER_MARKER)
        codegen += len(CODEGEN_FENCE_RE.findall(data))
    print(f"[recomp-patches] guest barriers: {added} inserted now, {total} present"
          f" (+ {codegen} emitted by codegen)")
    return added


# CR-08 fast MSR sections (docs/cr-fixes-20261007.md). Every guest
# `mtmsrd r13,1` ... `mtmsrd rX,1` (interrupts off/on) takes the SDK's global
# recursive mutex (REX_ENTER/LEAVE_GLOBAL_LOCK), and every `mfmsr` locks it to
# read a counter. Sections whose every path from the enter to its leave holds
# only lwarx/stwcx. on one word plus register-only instructions (compare,
# branch, arithmetic; no other load/store, call, return or nested section)
# are atomic by themselves (value-CAS), so their enter becomes
# FABLE2_MSR_ENTER_ATOMIC (src/core/fable2_guest_memory.h): with
# FABLE2_FAST_MSR on it skips the mutex; with it off it is the original
# REX_ENTER_GLOBAL_LOCK. All other sections keep the mutex. Idempotent: only
# remaining REX_ENTER_GLOBAL_LOCK() lines are analysed, and converted lines
# count as nested enters.
FAST_MSR_MARKER = MARKER + "fast-msr]"
FAST_MSR_OK = {
    "lwarx", "ldarx", "stwcx.", "stdcx.", "cmpw", "cmpwi", "cmplw", "cmplwi", "cmpd", "cmpdi",
    "cmpld", "cmpldi", "bne", "beq", "blt", "bgt", "ble", "bge", "b", "mr", "li", "lis", "addi",
    "addis", "add", "subf", "neg", "and", "andc", "or", "ori", "oris", "xor", "xori", "rlwinm",
    "clrlwi", "slwi", "srwi", "extsw", "extsh", "nop", "not", "nor", "mtmsrd",
}
_FUNC_START = re.compile(r"^(DEFINE_REX_FUNC|PPC_FUNC_IMPL|PPC_WEAK_FUNC|__attribute__)\b")
_LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+):")
_GOTO = re.compile(r"goto (loc_[0-9A-Fa-f]+);")
_CALL = re.compile(r"\b\w+\(ctx, base\)|REX_CALL_INDIRECT_FUNC|REX_LOAD_|REX_STORE_|\breturn\b")
_ENTER_LINE = "\tREX_ENTER_GLOBAL_LOCK();"
_ATOMIC_LINE = "\tFABLE2_MSR_ENTER_ATOMIC(); " + FAST_MSR_MARKER


def _section_is_atomic(lines, start, end, labels):
    """True if every path from lines[start] (the enter) reaches a leave through allowed lines only."""
    stack, seen, steps = [start + 1], set(), 0
    while stack:
        i = stack.pop()
        while True:
            if i in seen:
                break
            seen.add(i)
            steps += 1
            if steps > 400 or i >= end:
                return False
            line = lines[i]
            if line.startswith("}"):
                return False
            if line.startswith("\tREX_LEAVE_GLOBAL_LOCK();"):
                break  # this path left the section
            if "REX_ENTER_GLOBAL_LOCK" in line or "FABLE2_MSR_ENTER" in line or "REX_CHECK_GLOBAL_LOCK" in line:
                return False
            if line.startswith("\t// "):
                mnemonic = line[4:].split(None, 1)[0] if line[4:].strip() else ""
                if mnemonic not in FAST_MSR_OK:
                    return False
                i += 1
                continue
            if _LABEL.match(line):
                i += 1
                continue
            if _CALL.search(line):
                return False
            g = _GOTO.search(line)
            if g:
                target = labels.get(g.group(1))
                if target is None:
                    return False
                if line.lstrip().startswith("goto "):
                    i = target  # unconditional
                    continue
                stack.append(target)
            i += 1
    return True


def apply_fast_msr(files):
    converted = remaining = 0
    for path in files:
        with open(path, "rb") as f:
            data = f.read().decode("utf-8")
        if "REX_ENTER_GLOBAL_LOCK();" not in data:
            remaining += 0
            continue
        lines = data.split("\n")
        starts = [i for i, l in enumerate(lines) if _FUNC_START.match(l)] + [len(lines)]
        changed = False
        for a, b in zip(starts, starts[1:]):
            enters = [i for i in range(a, b) if lines[i] == _ENTER_LINE]
            if not enters:
                continue
            labels = {}
            for i in range(a, b):
                m = _LABEL.match(lines[i])
                if m:
                    labels[m.group(1)] = i
            atomic = [e for e in enters if _section_is_atomic(lines, e, b, labels)]
            for e in atomic:
                lines[e] = _ATOMIC_LINE
                changed = True
            converted += len(atomic)
        if changed:
            with open(path, "wb") as f:
                f.write("\n".join(lines).encode("utf-8"))
    total_atomic = total_enter = 0
    for path in files:
        with open(path, "rb") as f:
            d = f.read().decode("utf-8")
        total_atomic += d.count(FAST_MSR_MARKER)
        total_enter += d.count("REX_ENTER_GLOBAL_LOCK();")
    print(f"[recomp-patches] fast-msr: {converted} atomic sections marked now; "
          f"{total_atomic} atomic, {total_enter} locked sections present")
    return converted


def main() -> int:
    if os.environ.get("FABLE2_RECOMP_PATCHES") == "0":
        print("[recomp-patches] FABLE2_RECOMP_PATCHES=0 - skipping")
        return 0

    files = sorted(
        f for f in os.listdir(GENERATED_DIR) if f.startswith("fable_2_recomp.") and f.endswith(".cpp")
    )
    if not files:
        print(f"[recomp-patches] ERROR: no fable_2_recomp.*.cpp in {GENERATED_DIR}", file=sys.stderr)
        return 1

    applied_total = apply_startup_handoff(GENERATED_DIR)
    applied_total += apply_guest_barriers([os.path.join(GENERATED_DIR, f) for f in files])
    applied_total += apply_fast_msr([os.path.join(GENERATED_DIR, f) for f in files])
    for name, anchor, replacement in PATCHES:
        marker = MARKER + name + "]"  # MARKER ends with a space
        found = False
        for fname in files:
            path = os.path.join(GENERATED_DIR, fname)
            with open(path, "rb") as f:
                data = f.read().decode("utf-8")
            if marker in data:
                found = True  # already applied (idempotent)
                continue
            if anchor not in data:
                continue
            data = data.replace(anchor, replacement, 1)
            with open(path, "wb") as f:
                f.write(data.encode("utf-8"))
            found = True
            print(f"[recomp-patches] applied '{name}' in {fname}")
            applied_total += 1
        if not found:
            print(f"[recomp-patches] ERROR: anchor for '{name}' not found in any "
                  f"{len(files)} file(s) under {GENERATED_DIR}", file=sys.stderr)
            return 1

    print(f"[recomp-patches] done ({applied_total} applied now, rest already present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
