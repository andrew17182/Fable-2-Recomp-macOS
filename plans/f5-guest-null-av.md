# Plan: fix intermittent "Unhandled guest access violation: read of guest 0x00000000" on F5

**Symptom:** sometimes, when running the F5 external-Lua flow in game:

```
Unhandled guest access violation: read of guest 0x00000000 (host 0x0000000200000000) on thread 0xF800002C
```

## What the error actually means

- Emitted from `thirdparty/rexglue-sdk-src/src/system/xmemory.cpp:547`
  (`Memory::AccessViolationCallback`): a fault landed inside the guest memory
  mapping, the faulting guest address is **exactly 0** (host `0x200000000` =
  `virtual_membase_ + 0`), and guest page 0 is not committed on the *virtual*
  heap, so there is nothing to recover (recovery only exists for the physical
  heap / write-watch case).
- Translation: **guest (Fable 2) code executed a load through a NULL pointer.**
  This is a guest-side null dereference, not an emulator bug. The recompiler is
  faithfully faulting on a `lwz r?,0(r?)` with `r? == 0`.
- `thread 0xF800002C` = the guest thread that faulted (needs identification:
  main game thread vs. UI/worker — see Phase 0).
- The process then dies through the unhandled-exception path, and
  `exception_handler_win.cpp` (`LogUnhostedException`) *already prints the
  guest register file and guest `lr`* (`"Unhosted exception: ... gpr: ... lr="`)
  for exactly this case. That `lr` identifies the faulting guest function.
  Those lines live in the console output right after the "Unhandled guest
  access violation" line — a captured repro is all that's needed to pin the
  exact instruction.

## F5 pipeline (recap)

1. `fable2::f5lua::poll_f5()` (host input poll, `src/input/keyboard_gamepad.h`)
   edge-detects F5 → latches `g_f5_pending`.
2. `poll_mainloop()` in the `MainRenderLoop_82B9CD68` hook (guest main thread)
   consumes it → `run_external()`:
   - guest-allocs a C-string + a zeroed 64-byte String object,
   - `ConstructRefCounted_8222CF18` builds the game String,
   - calls the **pinned** `(g_this, g_method)` = `CScriptManager::RunScript`
     (method `0x825ADB40`, named `LuaScriptError_FormatMessage` by the
     recompiler; verified to be the run-a-`.lua` dispatcher),
   - deliberately leaks the String so the *deferred* file load can't hit a
     use-after-free (previously observed as "read of guest 0x5" AV).
3. `LuaBind_RunScript_82806168` probe pins `(this, method)` **once per process**
   from the first `.lua` dispatch — which at boot is the game's own startup
   script `miscellaneous/GeneralScriptManager.lua`.
4. `src/lua/F5.lua` registers a persistent per-frame coroutine
   (`GeneralScriptManager.AddScript`) that opens a native menu box, then
   `loadfile`+`pcall` the chosen script (e.g. `getPlayerPos.lua` →
   `GUI.DisplayMessageBox`).

## Root-cause candidates (ranked)

### A. Stale pinned `CScriptManager` `this` (most likely)

`g_captured_once` pins `g_this` exactly once, from a boot-time dispatch.
`fable2_f5_lua.log` (append mode across runs) shows the manager instance
address is **not stable**: `0x4215A8B0`, `0x4215F9F0`, `0x4215AD90` — all in the
same small front-end heap region, i.e. the object is allocated/freed and
re-allocated at nearby addresses. If the game recreates the script manager at
any point during a session (scene/mission transition, front-end rebuild, debug
reload), the pinned `this` dangles. `RunScript` then reads members of
freed/reused memory; when a member (vtable head, script table, string) is
zeroed, the next indirect load faults at **guest 0** — exactly the observed
error, and "sometimes" = dependent on where in the game you press F5.
The call itself (`lwz r3,0(r26)` in `0x825ADB40` → `sub_824154F0`) reads
`*(this)` first, a prime null-propagation path.

### B. Modal overlap in F5.lua (second most likely)

`update()` guards only the *awaiting-selection* phase (`st.menu_open`) and a
fixed 30-frame cooldown after the menu closes. It does **not** guard:
- F5 pressed while the previous `GUI.DisplayMessageBox` is still up (the
  message box stays open until the user presses A — well past 30 frames),
- F5 pressed while the menu box is still mid-teardown (one `coroutine.yield()`
  may not be enough; the code comment itself notes re-opening "on top of the
  live one (and re-running the script loader mid-modal) is what hard-crashed
  the game").

`GUI.DisplayMenuBox` while another modal owns the message pipeline can make the
game read a NULL "current box"/payload → null deref. Matches "sometimes":
only when the timing lines up.

### C. `getPlayerPos.lua` null-entity deref

`pcall` only catches **Lua** errors — it does not stop a C++ binding from
dereferencing a NULL hero entity. If the hero pointer is null/dying at bind
time (cutscenes, loading, death, front-end), `hero:GetPosition()` (or an
internal path in `DisplayMessageBox`'s entity context) reads guest 0. `pcall`
gives false safety here.

### Ruled out / unlikely

- **String object too small:** the game String is a single pointer
  (`obj->ref`, see `ConstructRefCounted_8222CF18.cpp`); our 64-byte zeroed
  alloc is ample.
- **Register/stack clobber by the hook:** `poll_mainloop` saves/restores the
  full `PPCContext`; the step1→step2→step3 log lines confirm the calls return
  cleanly.
- **Deferred-load string UAF:** already fixed (String deliberately leaked).
- **Emulator/xmemory bug:** guest 0 is uncommitted by design; the fault is a
  real guest load.

## Debug log: why F5 "did nothing" after the first fix (2026-09-26, resolved)

Two separate bugs were found while verifying:

1. **Lua 5.1 pcall/yield** — `pcall(open_menu)` converted the poll-loop's
   `coroutine.yield()` into an "attempt to yield across metamethod/C-call
   boundary" error (verified with a locally built Lua 5.1.4 interpreter).
   Fix: `open_menu` is called directly (yields escape into the coroutine);
   each non-yielding API call inside it is pcall'd individually.

2. **Wrong method captured (the real "nothing happens")** — the shared
   Lua→C++ dispatcher (`LuaBind_RunScript_82806168`) serves many bound
   methods whose string argument is a `.lua` path, not just RunScript. The
   "refresh on every dispatch (latest wins)" rule overwrote `g_method` with
   register-style methods (e.g. `0x825B27D0` from 'CameraFunctions.lua',
   `0x825B26A8` from 'AIHistory.lua'), so F5 called the wrong member fn with
   the path → no visible effect. The log showed all of it:
   `this=0x4215F9F0 head=0x4F640220` stable, method varying per dispatch.
   Fix: first `.lua` dispatch pins the canonical file-runner method (boot:
   `0x825ADB40` via 'miscellaneous/GeneralScriptManager.lua'); later captures
   refresh `this` only when the method matches; other methods are ignored.
   The stale-`this` protection (head plausibility + expected-head match) is
   kept as-is.

Verification tooling kept in `scratch/`: locally built Lua 5.1.4
(`scratch/lua-5.1.4/src/lua51.exe`, `luac.exe`) + `scratch/f5_sim.lua`, a
frame-stepping harness that runs the real F5.lua against stubbed
GUI/MessageEvents/GeneralScriptManager APIs. It confirmed F5.lua v2 is
logically correct: menu opens, selection runs the script, F5 during an open
message box is dropped, re-open after dismissal works, coroutine survives.

## Implementation status (2026-09-26)

Phase 1.1–1.4 + Phase 0.4 implemented:

- `src/core/fable2_f5_lua.h`
  - Capture: first `.lua` dispatch pins the canonical file-runner
    `(method, this)`; same-method dispatches refresh `this`; other methods
    with .lua args are ignored; logs `(re)captured this=.. method=.. head=..`.
  - Pre-call validation in `run_external()`: `this` readable; `head = *(this)`
    non-zero, non-0xFFFFFFFF, plausible (RAM readable or guest code);
    `head == g_expected_head` (the word captured with the instance — catches
    freed/reused blocks whose new first word is still "plausible"). Any
    failure logs and skips the run; the next `.lua` dispatch refreshes.
  - `run #%u state='<stateprobe name>'` logged (flushed) before each RunScript
    call; `step1` line now includes `head=`.
  - Forward-declares `fable2::stateprobe::CurrentStateName()` (defined later
    in main.cpp's include chain).
- `src/lua/F5.lua`
  - `st.menu_open` + fixed cooldown → `st.busy` + modal-dismissal tracking:
    a modal is "clear" once `GetMostRecentMessageID()` advances past the
    anchor (box dismissal posts a message) AND ≥10 frames elapsed, hard cap
    300 frames. F5 presses are dropped while `st.busy`. Re-anchors after the
    menu result AND after the chosen script runs, so a `DisplayMessageBox`
    left open by the script keeps F5 inert until it's dismissed.
- `src/lua/getPlayerPos.lua`
  - Best-effort live-hero guards (pcall-wrapped lookups, userdata type check,
    optional `IsValid()` probe, userdata check on the position result). Note:
    pcall cannot stop a C++ null-entity deref inside the binding; the guards
    only avoid calling it with an obviously-dead reference.

Phase 0 (crash-log capture) + Phase 2 (repro matrix) remain: run the game
with the console visible, trigger the AV, and read the `lr=` from the
`LogUnhostedException` block right after the error line to confirm which
subsystem faulted.

## Plan

### Phase 0 — capture ground truth (one repro)

1. Reproduce with the console visible (or `tools/run_timed.ps1`-style log
   capture). Grab the **full** output from the "Unhandled guest access
   violation" line onward — `LogUnhostedException` prints
   `Unhosted exception: code ... at host PC ...` + `guest ctx` + `gpr:` +
   `lr=`.
2. Map the faulting guest `lr` (and `r4`/`r11` bases in the `gpr:` lines) to a
   function in `generated/default/fable_2_recomp.*.cpp` /
   `generated/default/fable_2_funcs.h`. This tells us *which* subsystem
   (script manager / message box / hero binding) read NULL and confirms or
   kills candidates A/B/C.
3. Note whether the faulting thread is the main game thread (points at A/C or
   the modal pipeline) or a worker (points at state our script changed,
   handled by a UI thread).
4. Cheap instrument to make every future crash self-explanatory (2–3 lines):
   in `run_external()`, log the `fable2_state_probe` game state + a monotonic
   F5 counter into `fable2_f5_lua.log` just before calling RunScript. The log
   is flushed per line, so the last line before the crash is always recorded.

### Phase 1 — harden the F5 path (DONE — see implementation status above)

1. Stop pinning once — refresh `g_this`/`g_method` on every `.lua` dispatch.
2. Validate `this` + `head` (incl. expected-head match) before calling.
3. `F5.lua`: `st.busy` + message-ID modal-dismissal tracking replaces the
   fixed cooldown gate.
4. `getPlayerPos.lua`: best-effort live-hero guards.

### Phase 2 — verify

Repro matrix (each on release + debug builds):
- F5 spam (hold key) in mid-mission.
- F5 while the position message box is open; F5 within a frame of closing it.
- F5 immediately after cancelling the menu; double-selection back-to-back.
- F5 across a scene transition / mission load / pause-menu round trip
  (validates candidate A — stale `this`).
- F5 from the front-end / during a cutscene (validates candidate C).

Success = no `Unhandled guest access violation` in any case, F5 menu still
opens/runs scripts normally, and `fable2_f5_lua.log` shows the pre-call
validation lines passing.

## Fallback if Phase 0 points elsewhere

If the faulting `lr` lands in code unrelated to the script manager, message
boxes, or hero bindings (e.g. a worker thread), the state log from Phase 0.4
plus the `gpr:` dump identify the corrupted object; at that point suspect
heap-neighbor corruption from the leaked per-press allocations (unlikely: they
are small, heap-allocated via the game's own `Allocate_SizeBucketed`) and
extend the AV diagnostic in `xmemory.cpp:547` to also print the host PC /
guest `lr` inline so the two log lines can't be separated.
