# Proactive telemetry design — 2026-10-08

Goal: when the next playtest bug arrives, the evidence should already be on disk from the run
where it happened, so the investigation does not need a new probe, a new harness and two or three
more reproductions.

Sections 1–14 are the design as written before any code. **Phase 1 is built: see §15 "Implemented in
phase 1"** for what exists, the switch reference, the validation results and where the build differs
from the design. Scratch notes with the line references behind the design's claims:
`out/validation/telemetry-design-20261008/notes.txt` (line numbers there predate the cleanup commit
and the area-load fixes; §15 lists the current ones).

**Evidence labels.** **[V]** means I read it in the code or counted it in session data. **[I]** means
it is inferred and still needs checking. Anything not labelled is a design proposal.

## 1. Summary

The design has five parts. Each one fixes a pain point that recurs across the dated docs.

1. **Flight recorder.** An always-on recorder keeps a per-thread lock-free ring of 32-byte binary
   events for the last ~30–60 s: frames, GPU, guest waits, I/O, log lines, state tags and a
   100 Hz all-thread guest stack sampler. It writes to disk only when a trigger fires or on demand.
   The 30 s trace windows and line caps go away, and the first AV can no longer be rotated out of
   the log.
2. **Detectors.** A recorder thread watches for:
   - a frame gap;
   - the first guest AV, or a "GPU timeout" or "persistent miss" log line (through a log sink);
   - a long or timed-out guest wait;
   - draw-skip bursts;
   - a GPU stall;
   - a long load.

   When one fires, it dumps the rings plus an all-thread guest stack snapshot without suspending the
   threads, and optionally a full-resolution capture that includes the ImGui overlay.
3. **Game-state tags.** Every event is labelled with cheap state: loading, level-load, save notice,
   the region path from file opens, Bink movies, the UI signature, turbo and FPS mode. A timeline
   reads "Bower Lake load, frame 4120" instead of "t=17.75".
4. **One switch, one file layout, one tool.**
   - `FABLE2_TRACE=cat:level,...`. The old variables stay as aliases.
   - Per session: `trace/` holding `session.json`, `stream.f2t`, `dumps/`, `log-digest.jsonl`.
   - `tools/f2diag/`, a library and CLI that replaces ~100 copy-pasted harness scripts.
5. **Bug report (F9).** The bug report is a flight-recorder dump with reason `user`, plus the
   frame ring (1–2 s of small frames), a full-resolution overlay capture, settings and location.

**Phase 1** (size L in total, split into S/M items, §10): the recorder core, frame-gap and first-AV
triggers with all-thread guest stacks, game-state tags v1, the loading-trace / lr-sampler /
stack-sampler moves onto the recorder, `trace dump|mark` in test control, and `f2diag` v1. It is
validated in one run on the open Bower Lake 5 s stall: the automatic dump should name the 3D Engine
poll loop's caller chain without any new probe.

## 2. Inventory of what exists

| Probe | Switch | Captures | Output | Overhead | Limits / distortion |
|---|---|---|---|---|---|
| Frame CSV (`metal_frame_log.cc`) | cvar `metal_frame_logging`, **on by default** [V] | `guest_swap`, `guest_ready`, `present_submit`, `presented`, and every `Record()` from other traces | `logs/frames-<ms>-<pid>.csv` (event, frame, time_us, duration_us, warmup) | Mutex + vector per event, writer flushes every 500 ms. Untraced: ~200 rows/s [V u1] | 32,768-event pending cap per 500 ms, then a `dropped` row. Last ≤0.5 s lost at `_Exit`. Clock is mach uptime minus its own origin |
| GPU time trace (`gpu_time_trace` in `metal_command_processor.cc:151-325`) | `FABLE2_GPU_TIME_TRACE=1` | Per-CB GPU start/end (`gpu_cb`, `gpu_cb_sa`); `tr_sync_wait_us`, `tr_slot_wait_us`, `tr_cp_idle_us`; per-frame CPU of 4 named threads (`tcpu_*`); `tr_n_*` dedup counters | Frame CSV rows | Traced 133–138 FPS against 135.5 untraced [V perf-headroom] | Thread lookup by name prefix (a name bug made it inert in run on3) |
| Guest lr sampler (`GuestSampler`, same file) | part of `FABLE2_GPU_TIME_TRACE` | `PPCContext::lr` of 3D Engine, GameThread and Audio Worker every 250 µs, without suspending them | `smp_*` per frame in the CSV; `[gpu-time-trace] guest lr top` log line every 10 s | One host thread at ~2.6 kHz | **Leaf return address only.** Fixed 3 threads, fixed wait-lr tables, 10 s histograms |
| Pass trace (`metal_pass_trace.cc`) | `FABLE2_GPU_TIME_TRACE=1`, `FABLE2_PASS_TRACE=0` turns off | Per-encoder stage timestamps, draws, barriers and fence waits, keyed by attachments | `logs/passes-*.csv` | 24 × 4096-sample buffers; CSV of 140 MB in 84 s [V t1] | Huge files. Only usable over a chosen window |
| Draw-skip trace (`DrawSkipTrace`, l.2741) | `FABLE2_DRAW_SKIP_TRACE=1` | Per-frame `ds_*` reasons, `tr_tex_*` per texture, `[shmem-valid-first]` | Frame CSV | `tr_tex_*` is 1.37 M of 1.55 M rows in 84 s [V t1] | Per-texture rows dominate the CSV |
| Draw dump (l.3570) | `FABLE2_DRAW_DUMP=<file>`, `_FRAMES=a-b` | Fixed binary record per draw (shaders, fetch constants, 256 VS constants, state) | `.bin`, read by `dumplib.py` | 2.1 GB for 900 frames [V] | The frame window must be guessed beforehand. Dump frame N = capture N+1 |
| Shared-memory check (l.3513) | `FABLE2_SHMEM_CHECK=1` | Guest RAM vs GPU buffer diffs | CSV + warnings | High | "Too noisy to use" [V 2026-10-07 playtest notes] |
| Loading trace (`src/core/fable2_loading_pacing.h`) | `FABLE2_LOADING_TRACE=1` | 100 ms per-thread CPU / guest-wait / NtReadFile profile; delays ≥2 ms; waits ≥5 ms with guest stacks; long waits ≥1 s; worker-0 ops; level-load enter/exit; poll-wait and set-event ring; dispatcher in-flight list | `[loading-pacing] …` log lines | ~200 lines/s from XrnmThread and `82D0135C` alone [V area-load] | **30 s windows** (l.83, 171, 192, 263), **6000-line caps**, level-load capped at **50 calls**, a `std::string` stack per logged call, shared atomics in hot hooks |
| Stack sampler (`src/core/fable2_stack_sample.h`, new) | `FABLE2_STACK_SAMPLE=1` or `FABLE2_LOADING_TRACE=1` | At entry of `ByteValueCheck_82CBC678` and `DelayMilliseconds_82CC2028`: ≤1 sample per 2 ms per thread, 6-frame back chain | `[stack-sample]` top-8 line per ~1 s | Global mutex + `unordered_map<std::string>` per sample | Only threads that call those two helpers. 6 frames |
| Crash diag (`fable2_guest_memory.h`) | `FABLE2_CRASH_DIAG=1` (PCH, static init) | Registers and object words before an invalid indirect call | `[crash-diag]` lines | Cold path only | Only invalid indirect calls, not AVs |
| SDK AV message (prebuilt runtime, `xmemory.cpp:547`) | always | "Unhandled guest access violation: read of guest 0x… on thread …" | main log | One line per AV | 675,034 lines and 20 rotations in one storm; **the first AV was lost** [V] |
| Ring/tick wait stats (`fable2_spin_wait.h`) | `FABLE2_GPU_TIME_TRACE=1` | 10 s summaries of sleeps, late wakes, ring waits, timeouts | log | negligible | 10 s granularity |
| Session journal (`fable2_session_journal.h`, `_telemetry.h`) | `FABLE2_SESSION_DIR` (test sessions) | UI-structure signatures, pad state at the `XamInputGetState` wrapper, keys, markers | `control/events.jsonl` | small | Test sessions only. The signature is a hash, not a named state |
| Test control (`metal_test_control.mm`, `tools/fable2_test_control.py`, MCP `fable2`) | `FABLE2_TEST_CONTROL_DIR` | capture (PPM + json with frame_id and CLOCK_MONOTONIC), press, focus, ping, close | `control/*.req/.reply/.ppm` | `CaptureGuestOutput` is synchronous on the UI thread | **`press` blocks the single request thread for the hold** [V]; captures guest frame only |
| Replay pad channel (`fable2_replay.py`) | `control/replay-pad.txt` | Pad state injected at the guest input boundary | — | — | Non-blocking (the area-load run used it to avoid the press block) |
| Session watcher (`fable2_dev_session.py watch`) | normal sessions | Save detection and copy, options and cache write-back, exit | `writeback.jsonl` (incl. `QuestName`, `LevelName` after a save) | 1 poll / `POLL_SECONDS` | Labels saves only; no live state |
| FPS meter / func trace / thread trace / lock trace / save trace / boot-save trace / skip-intro trace / clock report | `FABLE2_FPS_METER`, `FABLE2_FUNC_TRACE(_FILTER…)`, `FABLE2_TRACE_WINDOW/_DELAY/_DUR/_FILTER`, `FABLE2_THREAD_TRACE`, `FABLE2_WATCH_GUEST_LOCK`, `FABLE2_SAVE_TRACE`, `FABLE2_BOOT_SAVE_TRACE`, `FABLE2_SKIP_INTRO_TRACE`, `FABLE2_CLOCK_REPORT` | Narrow, one-purpose | Separate log files or log lines | Varies; the func trace is very high | Each has its own window or cap semantics |
| Menu-text era probes (font, glyph, text, deadbeef, heap scan, UI text dump, UI input, hotfuncs, state probe) | many `FABLE2_*` | Menu text research | Own log files | Off | Mostly Windows-era; the AV probe is `#ifdef _WIN32` [V main.cpp:29] |
| Harness scripts `out/validation/*/` (~110 .py/.sh) | per script | Launch + captures + per-thread CPU (libproc `threadcpu.py`, no attach) + analysis | per-run JSON/JSONL | `threadcpu` is non-distorting [V] | Copy-pasted: `sweep.py` ×3, `boot.py` ×3, `lp_analyze.py` ×2, `gpustats.py` ×2, `movecheck.py` ×2, `blue.py` ×2. They monkeypatch `subprocess.run` for game args and `prepare_session` for warm caches. Three clocks, aligned through a hard-coded offset |
| `/usr/bin/sample` | harness `WP_SAMPLE`/`AL_SAMPLE` | Host call stacks | `.sample.txt`, `sshare.py` | Suspends the task | **FPS drops to 4–28; GameThread yield share 79% → 18%** [V perf-headroom] |
| Tracy (`libTracyClient.dylib`) and SDK perf counters | — | — | — | — | **Compiled out in Release** (`$<NOT:$<CONFIG:Release>>`, SDK `CMakeLists.txt:27-31`), and the build is Release [V]. The library is linked but inert |

## 3. Pain points

| # | What happened | Source | Fixed by |
|---|---|---|---|
| P1 | Loading trace stops at 30 s and at 6000 lines (level-load at 50 calls); a later area transition would be cut off | area-load "Trace window" | Recorder rings (no windows) + stream mode |
| P2 | The 5 s / 10 s Bower Lake stall could not be located. The lr sampler shows only leaf helpers, `sample` was distorted, and the conclusion was "instrumentation first", then 2–3 more reproductions | area-load §4.1 | Always-on 100 Hz multi-frame stack sampler + frame-gap trigger |
| P3 | `/usr/bin/sample` suspends the task: FPS 4–28 and inverted wait shares | perf-headroom | Non-suspending guest walks; brief host-stack suspend only at a trigger |
| P4 | AV storm: 675k lines, 20 rotations, first AV lost; a crash-only check missed a live storm | 2026-10-05 playtest notes | Log sink → first-AV trigger + `log-digest.jsonl` that survives rotation |
| P5 | `press` with a hold blocks captures | area-load t1 | Non-blocking press (§8) |
| P6 | Captures contain only the guest frame; the turbo notice could not be verified | bug-patterns "Limits", session notes | Drawable capture after ImGui (§5.5) |
| P7 | 17–20 captures/s miss one-frame glitches; dump frame N = capture N+1 | 2026-10-07 playtest notes | Frame ring of every presented frame + frame ids in every event |
| P8 | Blind 2.1 GB draw dump; 4.5 GB of dumps to clean up | 2026-10-07 playtest notes, session notes | Optional draw-record ring dumped on a trigger (phase 4) |
| P9 | Whole runs wasted on silently inert traces (enable check cached too early; thread-name bug) | perf-headroom runs table | Hook heartbeat in every dump and in `session.json` |
| P10 | Three clocks, a hard-coded wall-minus-monotonic offset, per-save pixel-mean "loading screen" heuristics | `lp_analyze.py`, `phases.py` | One time base with clock pairs; state tags |
| P11 | Each code-review finding asks for its own one-off counter | code-review CR-06/07/09/13… | `FR_COUNT()` named per-frame counters, always on |
| P12 | Dev Launcher can't pass env vars | 2026-10-07 playtest notes | Launcher "Diagnostics" section |
| P13 | Harness copy-paste and monkeypatches | `out/validation` | `tools/f2diag` library |
| P14 | Hard exit: no on-exit dump; the last 30 s of pipelines are lost on close | session notes 10-07 | `OnWindowCloseRequested` + test-control `close` dump; signal-safe crash write |
| P15 | Run-to-run variance (2–21 flashes) forced 4+4 matrices | bug-patterns "Lessons" | Mechanism counters always on |
| P16 | A save-state capture was taken while the replay was still running | save-state work | `state` / `wait-for` in test control |
| P17 | `sheet.py` needs PIL, which isn't installed | area-load | The game writes PNGs (ImageIO `WritePng` exists) |
| P18 | Audio Worker burn: 74% of its lr samples are 0 (host code), invisible from guest probes | weak-mac-perf | Host stacks at a trigger (brief suspend) |

## 4. Bug classes we will probably hit next, and the probe that catches each in one run

| Class | One-run probe (category) | Hook | Notes |
|---|---|---|---|
| Audio underruns / crackle | `audio`: guest audio-frame cadence; gap > 2× period = event; count per second | Executable override `__imp__XAudioSubmitRenderDriverFrame` [V import exists] | **Host-side** queue depth lives in the prebuilt runtime's audio driver [I], so it isn't visible without a runtime rebuild. Guest cadence plus Audio Worker CPU is the proxy |
| Audio latency / desync in movies | `audio` + `movie`: frame times around `.bik` opens | Same + NtCreateFile path tag | Turbo drift is known and expected (audio is host real-time) |
| XAudio voice counts | `audio`: live XMA contexts | `__imp__XMACreateContext` / `__imp__XMAReleaseContext` [V imports] | PCM voices are invisible [I] |
| Input latency (pad → frame) | `input`: pad change at the guest boundary → next guest swap → `presented` | `ProcessAndProcessAndProcess1013_822B2D60` (`fable2_state_probe.h:715`) [V]; SDL event timestamp in `controller_hotkeys.h` / `window_keyboard_mouse.h` | The tool computes SDL→guest→swap→present per change |
| Memory growth / leaks | `mem` at 1 Hz: host `phys_footprint` (`task_info TASK_VM_INFO`), guest allocation imports (calls, bytes) | `__imp__NtAllocateVirtualMemory`, `__imp__NtFreeVirtualMemory`, `__imp__MmAllocatePhysicalMemoryEx`, `__imp__MmFreePhysicalMemory` [I: names to confirm in generated code], Metal heap pool sizes (`metal_heap_pool.cc`) | Trigger: RSS +500 MB over 10 min |
| Streaming / texture residency | `tex` per frame: creates, CPU/GPU loads, not-resident counts (aggregated, not per texture) | Existing `TexTrace` sites `metal_texture_cache.cc:1373, 3124, 4315, 4358` + `tr_tex_not_resident_*` | Also the Texture Streamer thread state from the sampler |
| Save/load integrity | `save`: content create/close timing, bytes written, result; watcher decode of the written save | `src/core/fable2_fast_content_io.h`, `fable2_quick_save.h`, `fable2_save_trace.h` hooks; `writeback.jsonl` | Trigger: content op failure or a save that the watcher can't decode |
| Quests / scripts: Lua errors and stalls | `script`: RunScript calls; Lua error messages | `LuaBind_RunScript_82806168` [V]; the guest Lua error path is **not located yet** (needs string-xref RE: `luaG_errormsg`/`luaD_throw`) [I] | A script stall shows as a GameThread sample cluster in Lua code |
| Physics / combat frame spikes | `frame` + sampler: frame time > 2× median = spike event; the sampler ring already holds the GameThread stacks | `IssueSwap` + sampler | No new hook; the tool shows the stack histogram for spike frames vs normal frames |
| Cutscenes and Bink | `movie`: `.bik` open/close, frame cadence during playback | NtCreateFile path hook (`fable2_skip_intro.h` `open_hooked`) [V]; `NtClose` handle match | In-engine cutscenes: letterbox detection from frame-ring luma [I] |
| Networking / Xbox Live stubs | `net`: first call per import + count + result | `__imp__NetDll_*`, `__imp__XamSession*`, `__imp__XMsgStartIORequest` (app/message ids) [V imports] | Catches the game blocking on a stubbed call (wait on a never-signalled handle shows up in the waits) |
| Crashes | Fatal signal → signal-safe ring write; log digest | `sigaction` chain (§5.6) | Host PC + module offset |
| Hangs / deadlocks | Watchdog: no guest swap, no UI paint, no GPU progress for 3 s → all-thread guest + host stacks, repeat at 10 s and 30 s | Recorder thread; `fable2_ring_progress_seq` export [V] | Host stacks show SDK lock waits (`__psynch_mutexwait`) |

## 5. Flight recorder

### 5.1 Where it lives

- Core: `src/diagnostics/fable2_flight_recorder.h` and `.cpp`, compiled into the executable. No
  SDK dependency in the ring code, so the Metal plugin can share it.
- The executable exports a C API, `fable2_fr_api()`, which returns a struct of function pointers
  (`register_thread`, `type_id`, `emit_slow`, `trigger`, `frame_id_ptr`).
  - `librexgpu-metal.dylib` resolves it once in `MetalCommandProcessor::SetupContext` with
    `dlsym(RTLD_DEFAULT, "fable2_fr_api")`. This mirrors the existing reverse lookup of
    `fable2_ring_progress_seq`.
  - [I] The executable must actually export the symbol. Check with `nm -gU fable_2 | grep fable2_fr_`.
    Fallback: put the core in the plugin and look it up from the executable, which is the
    direction already proven.
- Hot-path writes are inline. Each thread caches its own ring pointer in a `thread_local`, so an
  event costs no cross-module call.

### 5.2 Event schema (binary, fixed 32 bytes)

```c
struct FrEvent {          // 32 bytes, little-endian, 8-byte aligned
  uint64_t t;             // mach_absolute_time() ticks (timebase + clock pairs in the header)
  uint16_t type;          // registered event type (name, category, level, arg names in the header)
  uint8_t  thread;        // recorder thread index (header: host tid, pthread name, guest thread id)
  uint8_t  flags;         // 1 begin, 2 end, 4 followed by STACK records, 8 torn, 16 from signal context
  uint32_t frame;         // current guest swap id (global, relaxed load)
  uint32_t a;             // e.g. guest lr / handle / reason id / string id
  uint32_t b;             // e.g. r3 / result / count / sampled-thread index
  uint64_t v;             // duration ns, bytes, value
};
// Continuation records (same 32 bytes; type = FR_STACK or FR_TEXT):
//   FR_STACK: uint8 n, uint8 seq, then up to 7 big-endian-agnostic u32 guest return addresses
//   FR_TEXT:  uint8 len, uint8 seq, then 26 bytes of UTF-8 (log lines, file paths, tag values)
```

- One schema serves frames, waits, log lines, samples and tags.
- Stacks are raw `u32` return addresses, never strings. Today's `guest_stack()` builds a
  `std::string` on every logged call.
- The tool symbolises the addresses offline from `generated/default/fable_2_funcs.*.h`. Every name
  ends in its address (`YieldAndCheckThreshold_82CBD098`) [V], so a sorted start-address table is
  enough.
- Type ids are registered at static init: `FR_TYPE(load_level, "load", 1, "r3", "", "dur_ns")`.
  An unregistered or disabled type compiles to one relaxed load and a branch.

**Export formats.** The ring dump is the binary `.f2t` file. `f2diag export --jsonl` and
`--perfetto` (Chrome trace JSON, opens in ui.perfetto.dev) are produced from it. JSONL is never
written in the hot path.

### 5.3 Data structures and thread safety

- **Per-thread ring.** Single producer, power-of-two capacity, `std::atomic<uint64_t> head`.
  - The writer fills the slot and then does `head.store(h+1, release)`. There are no locks and no
    read-modify-write.
- **Registry.** A fixed array of 256 `Ring*`, registered under a mutex on a thread's first event,
  which is rare. Rings are never freed.
  - Guest threads that exit keep their ring, so a dump still shows a thread that died.
    `OnGuestThreadExit` [V] marks it exited.
- **Reader (dump).** For each ring it reads `head` (acquire), copies the ring, then reads `head`
  again. It drops records with index < `head_after − capacity`, which were overwritten during the
  copy. Writers never wait.
- **Signal context.** The log sink can run inside the SIGSEGV handler: the SDK logs the AV from its
  handler [V]. That path only does a ring write plus an atomic trigger flag.
  - If the thread has no ring yet, it uses a static 4096-record emergency ring with a
    `fetch_add` index. No allocation happens in signal context.
- **Enable checks.** One global `uint64_t g_fr_cat_level` word (2 bits per category), read relaxed.
  It is parsed once before the first guest thread starts: call `Init()` from
  `Fable2App::OnPreSetup`. This avoids the "enable check cached before the frame log existed" bug.
  Hooks that run earlier read a "not initialised" sentinel and re-check.
- **No shared counters in hot hooks.** Today's loading trace does `fetch_add` on shared atomics
  (`logged`, `long_n`). Per-frame counters live per thread and are folded at the swap.

### 5.4 Memory budget

| Item | Size |
|---|---|
| Hot thread rings (GameThread, 3D Engine, GPU Commands, Audio Worker, UI): 32 k events | 5 × 1 MB |
| Other threads (VFS, streamer, job workers, Xrnm…): 4 k events, ~30 threads | ~4 MB |
| Sampler ring (§6.3): 100 Hz × active guest threads × 64 B, 60 s | 8 MB |
| Log-line ring (last 4 k lines, FR_TEXT) | ~2 MB |
| Frame ring for bug reports (§9): 120 frames at 384×216 BGRA in shared MTLBuffers | 40 MB (toggle) |
| **Total** | **~19 MB without the frame ring, ~59 MB with it** |

Event rate check: an untraced run has ~200 frame-CSV rows/s, and a fully traced run ~2.2 k/s once
per-texture rows are excluded [V t1]. Even 5 k events/s × 32 B is 160 KB/s, so the hot rings hold
≥30 s.

### 5.5 Dump contents and file layout

Every session (dev session, test session or normal launcher run) gets `trace/` next to the main log.
It uses the same "parent of `log_file`" rule `metal_frame_log.cc` uses [V].

```
<session>/test-state/logs/            existing: fable_2.log*, frames-*.csv, passes-*.csv
<session>/test-state/logs/trace/
  session.json         build id, git describe, binary sha, env, cvars, fable2_config.toml hash, launcher
                       toggles, Mac model, GPU, core counts, clock pairs (mach / CLOCK_MONOTONIC /
                       realtime) at start, enabled categories, hook heartbeat
  log-digest.jsonl     first occurrence (full line, time, thread, frame, tags) + running count of every
                       WARN/ERROR template (digits stripped) — survives log rotation
  stream.f2t           only when a category is at level 2+ (continuous, lossless unless a ring overruns;
                       overruns are recorded)
  dumps/NNN-<reason>/
    ring.f2t           all rings, header (types, threads, strings, clocks, tags, trigger)
    summary.json       trigger, tags, last 120 frame times, top waits, per-thread state, hook heartbeat
    stacks.txt         all-thread guest stacks (3 snapshots) + host stacks, symbolised by the tool on read
    screen.png         full-res drawable incl. ImGui overlay (when the presenter can provide it)
    frames/            frame-ring PNGs (bug report / artifact triggers only)
```

Dump writes run on the recorder thread at utility QoS. A trigger is rate-limited per reason:
cooldown 10 s, at most 20 dumps per session, at most 200 MB of dumps per session. A trigger that
fires again during the cooldown increments a count in the open dump's `summary.json`. An AV storm
therefore produces one dump, not 675,000.

**Overlay capture** (verified not possible today: the port and the SDK never set `framebufferOnly`,
so the `CAMetalLayer` default `YES` applies):

- When the frame ring or a screen capture is enabled, set `metal_layer_.framebufferOnly = NO` in
  `MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread`.
- In `MetalPresenter::PaintAndPresentImpl`, after `ExecuteUIDrawersFromUIThread` and `endEncoding`
  and before `presentDrawable`:
  - if a full capture is pending, blit the drawable texture into a shared `MTLBuffer` and complete
    it in `addCompletedHandler`;
  - if the frame ring is on, run a downscale pass from the drawable into ring slot `frame % 120`,
    plus a 1-texel luma and colour-mean reduce for state and artifact tags.
- [I] `framebufferOnly = NO` turns off lossless compression on the drawable. Measure the cost in one
  uncapped sweep; it is expected to be well under 1% at 60 FPS.

### 5.6 Exit and crash

- **Clean close.** Override `Fable2App::OnWindowCloseRequested()` [V virtual exists] and dump
  reason `exit` there. The `close` command in `metal_test_control.mm` dumps before it pushes
  `SDL_EVENT_QUIT`.
  - [I] It is not verified that a test `close` (an SDL quit event) reaches `OnWindowCloseRequested`;
    hooking both covers either path. The SDK then hard-exits (`std::_Exit(0)`) [V].
- **Fatal signals.** At startup, open `trace/crash.f2t` and keep the fd. In `OnPostSetup`, after the
  SDK installed its handlers, install handlers for SIGABRT/SIGTRAP and chain SIGSEGV/SIGBUS/SIGILL
  ahead of the saved SDK `sigaction`.
  - If the SDK handler returns because it handled the fault, do nothing.
  - On the fatal path, `write(2)` the header and every ring's raw memory (async-signal-safe), then
    restore and re-raise.
  - Read `exception_handler_posix.cpp` closely before implementing: it restores the original
    handlers on some paths [V l.470-485].
- **Process vanished.** The watcher already logs `game-exited` [V]. It adds `exit-unclean` when no
  `exit` dump exists, so the last `crash.f2t` or `stream.f2t` is the evidence.

## 6. Automatic triggers and detectors

All detectors run on one `FlightRecorder` thread at a 20 ms tick and QoS utility. It never suspends
anything for detection.

| Trigger | Condition (defaults; `FABLE2_TRACE_GAP_MS` etc. override) | Signal source |
|---|---|---|
| `frame_gap` | No guest swap for > 1000 ms **outside** the level-load call. Inside it, an event only | `IssueSwap` stores `last_swap_t` [V hook point l.2046-2050] |
| `hang` | No swap **and** no UI paint **and** no GPU progress for 3 s; repeats at 10 s and 30 s | swap time, `PaintAndPresentImpl` paint time, `fable2_ring_progress_seq()` [V] |
| `first_av`, `av_storm` | First log line matching `Unhandled guest access violation`; a storm when > 100/s | Log sink (§7.3) |
| `gpu_timeout` | Log line `GPU timeout` / `GPU sync timeout` [V l.1681, 1710] — or **earlier**: oldest committed command buffer not completed after 500 ms | Sink; commit/complete events from `EnsureCommandBuffer` + `addCompletedHandler` |
| `slot_wait` | `WaitForFrameSlotSubmission` > 100 ms | l.6693 |
| `wait_timeout` | A guest wait with timeout ≥ 1000 ms returns 0x102, or any wait > 5000 ms | `KeWaitForSingleObject_Guest` and the import wait hooks (`loading_pacing.h:129-133, 187`) |
| `draw_skip_burst` | ≥ 50 draws in one frame skipped as `ds_async_compiling`, `ds_geometry_pipeline_failed` or `ds_host_vs_no_mesh` | `IssueDraw` reason counters (§7.1) |
| `shader_miss` | A `persistent miss` log line (`metal_stage_compile_cache.cc:261`) or a `pipeline_wait` > 50 ms | sink; l.3312 |
| `long_load` | Level-load call > 3 s, or the loading tag set for > 20 s | `sub_825F3050` hook |
| `artifact` (optional, phase 4) | Frame-ring luma dip versus both neighbours (`dark.py` rule) or a blue-dominant spike (`blue.py`) | Presenter reduce pass |
| `user` | F9, test-control `trace dump`, MCP `dump` | §8, §9 |

### 6.1 What a dump collects

1. **Rings**: all of them, as written.
2. **All-thread guest stacks, without suspending** (§6.2): three snapshots 5 ms apart, plus the
   sampler histogram of the 2 s before the trigger.
3. **Host stacks** (default on, `FABLE2_TRACE=nohost` to disable). For each host thread, in turn:
   `thread_suspend`, read `ARM_THREAD_STATE64`, walk the frame-pointer chain with
   `mach_vm_read_overwrite`, `thread_resume`.
   - Each thread is suspended for well under 100 µs. [I] Measure it in phase 1.
   - No allocation, logging or locking happens while a thread is suspended, because it may hold the
     malloc lock. All buffers are preallocated. The recorder records `dladdr` module offsets after
     resuming.
   - This covers threads in host code, such as the Audio Worker's 74% `lr=0` (P18).
4. **Screen**: a full-resolution drawable capture (§5.5) when `frame_gap` or `user` fires and the UI
   thread is alive.
5. **Tags** and the **summary**.

### 6.2 Guest stack walk of other threads (no suspension)

- **Pattern.** It generalises `GuestSampler`, which already reads `PPCContext::lr` of other threads
  without suspending them [V].
- **Thread list.** `kernel_state()->object_table()->GetObjectsByType<XThread>()`, then
  `t->thread_state()->context()` and `t->thread()->name()`, cached and refreshed every 1 s.
- **Per thread.**
  - Read `lr` and `r1` through volatile loads.
  - Generated code keeps every guest register in `ctx` and stores `ctx.lr` before every `bl` [V].
    `ctx.r1` is therefore current at call boundaries [I: the compiler may delay the store inside a
    function, but not across a call].
- **Walk.** The same back chain as `InBootLoader`/`guest_stack` [V], with stronger guards:
  - `sp` stays inside the thread's guest stack range (`stack_limit ≤ sp < stack_base`). The range
    comes from the guest KTHREAD (+0x5C/+0x60 per `xthread.h`), read once per thread with
    `SafeRead` (`mach_vm_read_overwrite`) [I].
  - `prev > sp`, `prev − sp ≤ 1 MB`, the LR is in `[0x82000000, 0x84000000)`, at most 24 frames.
    Inside a validated range, plain `REX_LOAD_U32` loads are used, about 50 ns per walk.
  - Without a valid range, every load goes through `SafeRead`, about 2 µs each; that is acceptable
    in dumps but not in the 100 Hz sampler.
- **Torn chains.** The thread keeps running, so the chain can change during the walk. The flag
  `torn` is set when a guard fails mid-chain. Three snapshots at a dump; the summary reports the
  common prefix.
- **Cost.** About 20 threads × 3 × ~0.1 µs, which is negligible.

### 6.3 Always-on sampler (replaces the lr sampler and the stack sampler)

- **Rate.** 100 Hz by default; `sampler:3` gives 1 kHz, which matches the old 2.6 kHz lr-sampler
  usage.
- **Per tick and guest thread.**
  - If `lr` and `r1` are unchanged since the last tick, it bumps a "same" count.
  - Otherwise it writes one `SAMPLE` event (a = lr, b = sampled thread, v = state) plus 1–2 STACK
    records (8–14 frames).
- **State per sample.**
  - `WAIT`: the thread is inside a hooked kernel wait. The wait hooks set a per-thread
    `in_wait_since` in the registry.
  - `SPIN`: the lr is in a known poll set (the existing `IsWait` tables, `82CBD0A8`, `82CC207C`,
    `82CBC68C`, `822426E8`, `82B9BF98`…).
  - `WORK`: anything else.

  That is the per-thread "real work vs spin" split, for every thread, always.
- **Host CPU per thread.** `thread_info` at 1 Hz (as `prof::Sampler` does today) goes into `CPU`
  events, so the CPU and the samples line up.
- **Cost estimate [I].** 100 wakes/s × ~5 µs, plus ~20 walks × 0.1 µs per tick, is about 0.1% of
  one core. Phase 1 measures it with the recorder's own thread CPU.

## 7. Standard event categories and hook points

The level is the default when `FABLE2_TRACE` is unset. L1 is always on (ring), L2 streams to disk,
L3 adds detail.

| Category | Events | Hook (file : function) | Default |
|---|---|---|---|
| `frame` | `swap` (frame id), `guest_ready`, `present_submit`, `presented`, `paint` | `metal_command_processor.cc : MetalCommandProcessor::IssueSwap` (with `frame_log::BeginFrame`); `metal_presenter.mm : PaintAndPresentImpl`, the presented handler; `frame_log::Record` forwards every CSV event into the ring too | L1 |
| `gpu` | `cb_commit`, `cb_done` (GPU start/end), `sync_wait`, `slot_wait`, `cp_idle`, `pass` (L3) | `EnsureCommandBuffer` (`AddHandler` made L1), `FlushCommandBufferAndWait`, `WaitForFrameSlotSubmission`, `CommitStandaloneAndWait`, `PrepareForWait`/`ReturnFromWait`; `metal_pass_trace.cc` at L3 | L1 (pass L3) |
| `cpu` | per-thread CPU ms, 1 Hz at L1, per swap at L2 | recorder thread (`thread_info`), `gpu_time_trace::ThreadCpu` folded in | L1 |
| `sampler` | `SAMPLE` + STACK, state WAIT/SPIN/WORK | recorder thread (§6.3) | L1 100 Hz |
| `wait` | guest sleeps and waits ≥ 2 ms (L1) or all (L3), with timeout, result, handle, 4-frame stack; ≥ 50 ms gets a 14-frame stack | `src/core/fable2_loading_pacing.h : TimedWait` (`KeWaitForSingleObject`, `NtWaitForSingleObjectEx`, `KeWaitForMultipleObjects`, `NtWaitForMultipleObjectsEx`, `KeDelayExecutionThread`), `DelayMilliseconds_82CC2028`, `KeWaitForSingleObject_Guest`, `ProcessAndQueryWithByteCheck_82C6F848` (poll-wait), `ByteValueCheck_82CBC678` | L1 |
| `signal` | set-event / release-semaphore with stack, **only while a wait > 300 ms is open** (existing rule) | `FABLE2_SETEVENT_TRACE` hooks (`loading_pacing.h:454-464`) | L1 |
| `io` | `NtCreateFile` path (FR_TEXT), `NtReadFile` duration/bytes, VFS request timeouts | `fable2_skip_intro.h : open_hooked`, `loading_pacing.h : __imp__NtReadFile` | L1 (open paths), reads L2 |
| `worker0` | marshalled worker-0 op duration, caller, drain | `ProcessAndWaitMapStatus_82B49B50`, `CheckTimeoutCondition_82C08010` | L1 |
| `load` | `level_load` begin/end (r3), `loading` tag, `save_notice` sleep, `content` create/close | `sub_825F3050`; `DelayMilliseconds_82CC2028` with caller `824EC440`; `fable2_fast_content_io.h` | L1 |
| `ui` | loading-screen / save-notice / fade state, UI signature change, title fast events | `UIText_FrameRender` `FrameScope` signature (`fable2_session_telemetry.h`), `ProcessAndProcessAndProcess1723_826C63D8`, presenter luma (phase 2) | L1 |
| `shader` | translation, pipeline create (ms), `pipeline_wait`, persistent miss, archive save/load (bytes, ms, result) | `metal_pipeline_cache.cc : CreatePipelineFromHandle`, `WaitForPipeline`, `SerializePipelineBinaryArchive`, `MaybeSavePipelineBinaryArchiveAsync`, `InitializePipelineBinaryArchive`; `metal_stage_compile_cache.cc` miss | L1 |
| `draw` | per-frame skip counts by reason (one event per frame, nonzero reasons only); `tex` per-frame aggregates | `IssueDraw` (`draw_skip_reason_` always counted into a per-frame array; `DrawSkipTrace::Add` keeps the CSV at L2); `TexTrace` aggregated | L1 aggregated; per-texture L3 |
| `shmem` | stale-valid runs, memexport preserve/readback bytes and µs, collapse counts | `metal_shared_memory.cc` (l.295-301, 864), `tr_n_*` counters | L1 aggregated |
| `clock` | turbo on/off and scale, FPS mode, fps-auto decisions, game-tick sleep stats | `src/core/fable2_turbo.h : SetActive`, `fable2_hooks.cpp : fable2_hook_fps_60`, `fable2_fps_auto.cpp`, `fable2_spin_wait.h` | L1 |
| `input` | SDL key/pad event (host time), pad change at the guest boundary, hotkeys | `ProcessAndProcessAndProcess1013_822B2D60`, `controller_hotkeys.h` event switch, `window_keyboard_mouse.h` | L1 |
| `audio` | render-driver frame submit cadence, XMA context count, Audio Worker state (sampler) | `__imp__XAudioSubmitRenderDriverFrame`, `__imp__XMACreateContext`/`ReleaseContext` (new executable overrides) | L1 (phase 3) |
| `mem` | host `phys_footprint`, guest allocation imports, Metal heap totals, 1 Hz | recorder thread; import overrides (phase 3) | L1 |
| `log` | every log line ≥ INFO (FR_TEXT), digest | `rex::logging::AddSink` in `Fable2App::OnPostInitLogging` [V both exist] | L1 |
| `count` | `FR_COUNT("name", n)`: named per-frame counters, flushed at the swap | anywhere; replaces one-off trace counters (P11) | L1 |
| `net` | first call + count + result per NetDll/XamSession/XMsg app message | import overrides (phase 3) | L1 |
| `script` | RunScript path; Lua errors (after RE) | `LuaBind_RunScript_82806168` [V] | L1 (phase 3) |
| `mark` | harness markers, hotkey markers | test control `trace mark`, F9 | L1 |

### 7.1 Notes on the expensive ones

- **Waits at L1.**
  - Every hooked call takes two `mach_absolute_time()` reads, about 10–20 ns.
  - Only calls ≥ 2 ms, timed-out calls, or calls with a timeout ≥ 1000 ms write an event.
  - The Sleep(0) poll loops call `DelayMilliseconds` very often (the 3D Engine stall loop). The cost
    there is the two timestamps, and the loop is spinning anyway.
  - Stack capture uses raw `u32` walks (~50 ns) instead of `guest_stack()` strings.
- **Draw-skip reasons.** `draw_skip_reason_` is already set on every path [V l.2796-3923]; counting
  it into a `uint16_t[kReasons]` array on the GPU Commands thread costs nothing measurable.
  - Per-texture `tr_tex_*` rows become per-frame aggregates at L1. Per-row output stays at L3.
- **GPU completion handlers.** These turn always-on: `addCompletedHandler` on each command buffer,
  a few per frame.
  - [I] Cost is a few µs per CB. It needs the overhead check.

### 7.2 Game-state tags

Tags are small state values. Each change writes a `TAG` event (a = tag id, b = int value, FR_TEXT for
strings). The recorder keeps a "current tags" table that it copies into every dump header,
`summary.json` and the test-control `state` reply.

| Tag | Source | Cost |
|---|---|---|
| `loading` (0/1), `level_load` (0/1) | `sub_825F3050` enter/exit; loading screen = level-load begin → first gameplay swap after the frame-gap/black phase (presenter luma, phase 2) | per call |
| `region` | Last level-data directory opened: the `NtCreateFile` path filter in `open_hooked`, for example `…\levels\…` [I: confirm which files mark a region change] | per open |
| `level_name`, `quest` | From the session watcher after each save (`writeback.jsonl` already has `LevelName`, `QuestName` [V]), sent back with test control `trace tag`; also from `chaptersave.bin` at launch (`describe_save`) [V] | per save |
| `rich_presence` | `__imp__XMsgStartIORequest` override: app 0xFB message `0x000B0006` (XGIUserSetContextEx: context id, value) [V SDK layout] | rare. [I] Whether Fable sets region contexts is to be checked in one run |
| `save_notice` | `DelayMilliseconds_82CC2028` with caller `824EC440` (the minimum-display sleep) [V area-load]; content create/close | per save |
| `movie` | `.bik` path in `open_hooked`, cleared on close | per movie |
| `ui` | UI signature `UI:<n>:<hash>` from `FrameScope` [V]; a small table maps known hashes to names (title, main menu, pause, inventory), grown as we see them | per UI frame (hash only on change) |
| `front_end` | `ProcessAndProcessAndProcess1723_826C63D8` title state (+24) [V] | per title frame |
| `turbo`, `fps_mode`, `paused` | turbo `SetActive`, `fps_60` hook, the window-focus hooks (`OnWindowFocusChanged`) | on change |
| `save` | session.json: hero / library entry / target slot | at start |
| `screen` | presenter 1-texel luma: black / white / static (frame-diff 0) | per frame (phase 2) |

These tags replace the pixel-mean `LOADSCREEN_MEAN` / `GAMEPLAY_MEAN` constants in `lp_analyze.py`
and the `title ready` / `loadscreen_last` heuristics (P10).

### 7.3 Log sink

- `class FrSink : spdlog::sinks::base_sink<null_mutex>`, added with `rex::logging::AddSink` in
  `Fable2App::OnPostInitLogging` [V]. For each message at INFO or above it does three things:
  1. writes FR_TEXT records with level, logger and thread into the thread's ring;
  2. matches trigger patterns (`Unhandled guest access violation`, `GPU timeout`,
     `GPU sync timeout`, `persistent miss`, `Failed to create pipeline state`, `materializeAll`,
     `FATAL`, `[crash-diag]`) and sets atomic trigger bits;
  3. updates a digest: template = message with hex and decimal runs stripped. The first occurrence is
     kept in full and the rest are counted. The digest is held in a fixed-size open-addressing table
     and flushed to `log-digest.jsonl` by the recorder thread every second and on every dump.
- The SDK formats the message before it reaches the sink, so the sink adds a pattern search and a
  memcpy. With a `null_mutex` the sink itself takes no lock, so it is safe in the SDK's AV signal
  path. [I] spdlog may still take the logger's lock there, as it already does today.

## 8. Unification: one switch, aliases, launcher

**Switch.** `FABLE2_TRACE=<item>[,<item>…]`.

- An item is `cat[:level]`, `all[:level]`, `off`, `nohost`, `noring`, or `gap=<ms>`.
- Levels: `0` off, `1` ring (default for the core categories), `2` stream to `trace/stream.f2t`,
  `3` detail.
- Examples: `FABLE2_TRACE=load:3,wait:3`, `FABLE2_TRACE=all:2`, `FABLE2_TRACE=off`, which turns off
  everything including the ring, for overhead A/Bs.
- It is parsed once in `fable2::fr::Init()`, called from `Fable2App::OnPreSetup`. The parser is a
  header with no SDK dependency, so the PCH-included guest code (`fable2_guest_memory.h`) can use it
  too.

**Backwards-compatible aliases.** These are kept; each one expands into `FABLE2_TRACE` items before
parsing, and a log line says what it expanded to.

| Old switch | Expands to | Behaviour change |
|---|---|---|
| `FABLE2_LOADING_TRACE=1` | `load:3,wait:3,io:2,worker0:2,signal:1,cpu:2,sampler:3` | No 30 s window, no 6000/50 caps. The `[loading-pacing]` log lines stay at L3 for existing scripts, and the time-window constants become `FABLE2_TRACE_LOG_SECONDS` (default unlimited) |
| `FABLE2_STACK_SAMPLE=1` | `sampler:3` | The entry-of-helper sampler becomes the always-on sampler; `[stack-sample]` lines are kept at L3 |
| `FABLE2_GPU_TIME_TRACE=1` | `gpu:3,cpu:2,sampler:3,frame:2` | `smp_*`/`tcpu_*` CSV rows stay. The lr histogram line is derived from the sampler ring |
| `FABLE2_PASS_TRACE=0` | `gpu.pass:0` | Unchanged |
| `FABLE2_DRAW_SKIP_TRACE=1` | `draw:2,tex:3,shmem:2` | Unchanged CSV rows |
| `FABLE2_SHMEM_CHECK=1` | `shmem:3` | Unchanged |
| `FABLE2_DRAW_DUMP=<f>` (+`_FRAMES`) | unchanged; phase 4 adds `draw:4` = in-RAM draw-record ring of the last 30 frames, dumped on a trigger | — |
| `FABLE2_CRASH_DIAG=1` | `crash:3` | PCH static-init read stays, and also accepts `FABLE2_TRACE` |
| `FABLE2_SKIP_INTRO_TRACE`, `FABLE2_SAVE_TRACE`, `FABLE2_BOOT_SAVE_TRACE`, `FABLE2_CLOCK_REPORT` | `io:3`, `save:3`, `load:3`, `clock:3` | Log lines kept |
| `metal_frame_logging` cvar | `frame` CSV output on/off; the ring is unaffected | — |

**Launcher** (`tools/macos/DevLauncher/main.swift`, per the "every setting gets a launcher control"
rule):

- a "Diagnostics" group with "Flight recorder" (on);
- "Frame ring for bug reports" (on for the maintainer);
- "Trace preset" (Off / Loading / GPU / Everything);
- a free-text "Extra env" field (P12).

## 9. Bug report (F9) on the recorder

The pinned feature becomes a thin layer: **F9 = `trigger("user")` + extras**.

1. A flight-recorder dump with reason `user`: rings, all-thread stacks, tags, summary and log tail.
   That covers frame times, location (tags), the last frames' waits and loads, and every recent log
   line.
2. `screen.png`: the full-resolution drawable after ImGui (§5.5), so the overlay is included.
3. `frames/`: the frame ring of 120 presented frames (~2 s at 60 FPS) at 384×216, written as PNGs by
   `WritePng` (the ImageIO code `controller_hotkeys.h` already has [V]), plus `frames.json` with frame
   ids and luma. One-frame glitches like the dog-fur flash are in it (P7). The ring is GPU-side, so it
   costs nothing until F9 [I: measure].
4. `settings/`: `fable2_config.toml`, `fable_2.toml`, env, launcher toggles, `session.json` (save
   entry, slot), build id, Mac/GPU.
5. A notice: "Bug report saved" through the existing ImGui notice path (`fable2_turbo_notice.h` style).

Later, per the pinned plan: a note box in the Fable font, an optional save copy, a controller combo,
and for public builds a zip with user paths stripped and a prefilled GitHub issue link, never
auto-uploaded. The same bundle is what an agent reads first (§11).

## 10. Phased plan

Sizes: S ≈ half a day of agent time, M ≈ 1–2 days, L ≈ 3+ days. Every validation step uses 1–2
runs, counts the first launch after a rebuild, and relies on mechanism evidence (a dump that names
the cause, or a counter that fires), not on statistics.

### Phase 1 — recorder, two triggers, stacks, tags, three traces moved (L total)

| Item | Size | Content |
|---|---|---|
| 1.1 Recorder core | M | rings, registry, type table, `FABLE2_TRACE` parser + aliases, `session.json`, dump writer (`ring.f2t`, `summary.json`), exported API, plugin lookup, hook heartbeat |
| 1.2 Frame + GPU basics | S | `frame` events from `IssueSwap`/presenter; `frame_log::Record` mirrored into the ring |
| 1.3 Log sink | S | `AddSink`, FR_TEXT, `log-digest.jsonl`, `first_av`/`av_storm`/`gpu_timeout` triggers |
| 1.4 Sampler + stack walk | M | generalised `GuestSampler` (all XThreads, 100 Hz, 14 frames, WAIT/SPIN/WORK); 3-snapshot dump walk; `frame_gap` and `hang` triggers; host stacks behind a default-on switch |
| 1.5 Tags v1 | S | `level_load`, `loading`, `save_notice`, `movie`, `region` (path), `turbo`, `fps_mode`, `ui` signature, `save` |
| 1.6 Move three traces | S | `FABLE2_LOADING_TRACE` waits/delays/worker0/level-load → `wait`/`worker0`/`load` events (windows and caps removed); `FABLE2_STACK_SAMPLE` → sampler; the lr sampler → sampler (CSV rows kept) |
| 1.7 Test control | S | `trace dump [reason]`, `trace mark <text>`, `state` (tags JSON), non-blocking `press` (each press on its own thread; reply when done) |
| 1.8 `tools/f2diag` v1 | M | `.f2t` reader, symbolizer (`fable_2_funcs.*.h`), `timeline`, `gaps`, `threads`, `stacks`, `export --perfetto` |

**Risks.**

- Exporting the executable API: verify with `nm`; the fallback is §5.1.
- Torn cross-thread walks: guards plus 3 snapshots; the walker never dereferences outside a
  validated range.
- Signal-context sink: no allocation; emergency ring.
- Hot-path cost of the wait timestamps: §7.1.
- Dump storms: cooldowns and caps.
- The recorder thread competing on 5–6 P-core Macs: utility QoS, 20 ms tick.

**Validation (2 runs).**

1. **Mechanism run, untraced defaults.** Area-load `run.py` on save
   `hero1-dog-live_2026-10-07_18-49-47` (Thag's Cave → Bower Lake) with the new build, first launch
   after the build. Expected:
   - an automatic `frame_gap` dump at the 5–10 s stall;
   - `stacks.txt` showing the 3D Engine chain above `YieldAndCheckThreshold`/`DelayMilliseconds`
     (for example through `822E0508`), the same chain in the 2 s sampler histogram;
   - `tags.loading=1`, `region` = Bower Lake path;
   - `log-digest.jsonl` written.

   If the area-load agent has fixed the stall by then, use `trace dump` during the level-load call
   instead, and a `FABLE2_TRACE_SELFTEST=gap` build-in that blocks one guest frame for 1.5 s at a
   fixed time.
   - For `first_av`: `FABLE2_TRACE_SELFTEST=av` emits one synthetic log line with the SDK's exact AV
     text at +20 s. Expect a `first_av` dump and one digest row.
2. **Overhead run.** CR-fixes `sweep.py`, 30 s camera sweep, uncapped, `FABLE2_TRACE=off` vs the
   default.
   - Pass: uncapped FPS within ±1%.
   - `FlightRecorder` thread CPU (libproc `threadcpu`) < 0.5% of one core.
   - Recorder self-report in `summary.json`: events/s × ns/emit (a ctest micro-benchmark of
     `emit` in `tests/`) < 0.5% of process CPU.
   - One run per arm. Repeat only if FPS differs by more than noise (±1.5% between sweeps
     historically).

### Phase 2 — GPU, presenter, overlay, bug report v1 (M–L)

- **Content.**
  - `gpu` commit/complete events always on, plus the `gpu_timeout` early detector and `slot_wait`;
  - `shader` events and the `draw` per-frame skip counts with `draw_skip_burst` and `shader_miss`
    triggers;
  - `framebufferOnly = NO` behind the ring toggle, with drawable capture after ImGui and the
    120-slot downscaled frame ring with luma;
  - test-control `capture screen` and `capture ring`;
  - F9 bug report v1;
  - the fatal-signal ring write.
- **Risks.**
  - Drawable readback cost and the lost framebuffer compression.
  - Completed-handler overhead.
  - Sigaction chaining with the SDK handler. Read `exception_handler_posix.cpp` and test one forced
    host crash in a test build only.
- **Validation (2 runs).**
  - One sweep: FPS ±1%, and `capture screen` shows the turbo notice after `press f7`. This is the
    first overlay evidence ever.
  - One run with a cold shader cache copy: a `draw_skip_burst` dump fires during the first camera
    turn, and its counts match `ds_async_compiling` in the CSV.

### Phase 3 — subsystems (M)

- **Content.**
  - `audio`: submit cadence, XMA contexts;
  - `input` latency chain;
  - `mem` 1 Hz;
  - `io` reads at L1 aggregated;
  - `net` first calls;
  - `script` RunScript plus Lua error RE (string xrefs to the guest Lua error functions);
  - `rich_presence` tag check;
  - the long-load trigger.
- **Risks.**
  - New import overrides must forward exactly; use the `dlsym(RTLD_NEXT)` pattern [V].
  - Lua RE time is unknown.
- **Validation (1–2 runs).** One unmuted playtest-style run through a cutscene and a save. Expected:
  - `movie` and `save_notice` tags in the timeline;
  - audio cadence near 5.33 ms per frame [I: confirm the period];
  - an input-latency histogram;
  - RSS flat ±50 MB over 10 min idle.

### Phase 4 — tooling consolidation and extras (M)

- **Content.**
  - Fold the harness scripts into `tools/f2diag` (table below);
  - `f2diag` subcommands `phases`, `pacing`, `gpu`, `dark`, `blue`, `compare A B`;
  - MCP tools `dump`, `state`, `timeline`;
  - launcher Diagnostics group;
  - `draw:4` draw-record ring;
  - the GPU artifact detector;
  - retention: delete dumps older than 14 days, keep `user` dumps.
- **Risks.** `tools/fable2_dev_session.py` changes need the maintainer's direct approval (CLAUDE.md). Keep
  the launch helpers in `f2diag` until that is approved; the current monkeypatches show where.
- **Validation.** Re-run two old analyses (the area-load `frames.py` table and the flash `dark.py`
  count) through `f2diag` on existing sessions. The numbers should match the docs exactly. No new
  game runs are needed.

### Phase 5 — public bug report (S–M)

Zip, path stripping, issue link, privacy review (§9).

## 11. Tooling: `tools/f2diag`

`tools/f2diag/` is a package; `tools/f2diag.py` is the CLI. It is pure Python plus numpy, writes
PNGs through the game or `sips`, and does not need PIL (P17).

| Module | Functions | Replaces |
|---|---|---|
| `session.py` | `Session(path)`: `.log()` (lines on the session clock), `.frames()`, `.passes()`, `.captures()`, `.cpu()`, `.trace()`, `.dumps()`, `.writeback()`; one clock model from `session.json` clock pairs | `logt.py`, `lp_analyze.load`, hard-coded `DEFAULT_OFFSET`, `frames.py` loader |
| `f2t.py` | reader, ring merge, `events(cat, type)`, `stacks()`, symbolizer | — (new) |
| `run.py` | `launch(save, env, game_args, warm_cache, fps)`, `capture()`, `pad_hold()`, `press()`, `wait_for(tag)`, plans `sweep_default`, `sweep_fast`, `boot_capture`, `move_check`, `f5_save`, `area_transition` | `flash-20261007/sweep.py`, `cr-fixes/sweep.py`, `playtest-20261005/sweep.py`, `load-profile/boot.py`, `cr-fixes/boot.py`, `shadercache/boot.py`, `area-load/run.py`, `movecheck.py` ×2, `f5save.py`, `press.py`, `movesave.py` |
| `threadcpu.py` | moved as is (libproc, non-distorting) | `cr-fixes/threadcpu.py` |
| `timeline.py` | `phases()` (tags + level-load + gaps), `gaps(threshold)`, `per_thread(window)` (CPU, WAIT/SPIN/WORK shares, waits by call site), `stack_hist(window, thread)` | `lp_analyze.py`, `phases.py`, `phasecpu.py`, `timeline.py` ×2, `waits.py`, `pscpu.py`, `prof.py`, `callers.py`, `sampletree.py` |
| `pacing.py` | `pacing(window)`: fps, 1% low, p50/95/99, gaps | `pacing.py`, `health.py`, `sweepstats.py`, `tools/summarize_frame_log.py` (kept as a thin wrapper) |
| `gpu.py` | `gpustats(window)`, `passstats(window, by)`, `sample_shares(sample.txt)` | `gpustats.py` ×2, `passstats.py`, `sshare.py`, `idlecmp.py` |
| `images.py` | `ppm()`, `dark_dips()`, `blue_spikes()`, `count_events()`, `montage()`, `diff()` (movement check) | `dark.py`, `count.py`, `blue.py` ×2, `overview.py`, `sheet.py`, `analyze.py` (ppm_stats) |
| `drawdump.py` | `DT`, `load(path, frames)`, `diff_draws()` | `dumplib.py`, `diffdraws.py`, `cmpdump.py`, `dumpread.py` |
| `logscan.py` | `digest()`, `grep(pattern, window)`, the bug-patterns greps (`persistent miss`, `skipping`, `Failed to create`, `materializeAll`, `[crash-diag]`, AV) | `logcount*.sh`, `dscount.py`, `rbcount.py`, `loadtimes.py` |

CLI: `f2diag timeline|gaps|threads|stacks|phases|pacing|gpu|dark|digest|export <session>`. It also
has `f2diag report <dump-dir>`, which prints what an agent should read first: trigger, tags, the
gap, the top stacks per thread, waits in the window, and log digest rows near the trigger.
`docs/bug-patterns.md` triage step 2 becomes "run `f2diag report` on the newest dump".

## 12. Test-harness fixes

1. **Non-blocking press** (`metal_test_control.mm`). Run each `press` on a detached worker thread
   that writes the reply when done. Add `down <key>` / `up <key>`. Captures and other commands then
   proceed during holds [V: today the hold sleeps on the request thread].
2. **Overlay capture.** `capture screen` (after ImGui, §5.5) next to the existing `capture` (guest
   only). `capture ring` writes the last N ring frames.
3. **Trace commands.**
   - `trace dump [reason]`, `trace mark <text>`, `trace tag <k>=<v>` (the watcher pushes
     `level_name`/`quest` after saves);
   - `state` returns the tags, frame id, last gap and fps;
   - `wait-for <tag><op><value> <timeout>` replaces fixed `time.sleep(12)` waits and pixel-mean
     loading detection (P16).
4. **Python and MCP.** `fable2_test_control.py` gets `press(..., wait=False)`, `mark`, `dump`,
   `state`, `wait_for`; MCP `fable2` gets `dump`, `state`, `timeline`.
5. **Launch parameters.** `fable2_dev_session.launch` gets `game_args=` and `cache_from=`, which
   remove the two monkeypatches. This needs the maintainer's approval (CLAUDE.md).

## 13. Overhead and risks

| Risk | Mitigation |
|---|---|
| Cross-thread guest stack walk faults or reads garbage | Walk only inside the thread's validated guest stack range; `SafeRead` otherwise; the `InBootLoader` guards [V]; `torn` flag; 3 snapshots |
| Host-stack suspend deadlock (suspended thread holds malloc or log lock) | Preallocated buffers, no allocation, logging or locks while suspended; one thread at a time; `FABLE2_TRACE=nohost` |
| Atomics in hot paths | Single-producer rings (relaxed stores + one release store); no shared `fetch_add` in hooks; per-thread per-frame counters folded at the swap |
| Signal-context logging | Sink does a memcpy into a ring + atomic bits only; emergency ring |
| Overhead creep (categories turned to L1 without measurement) | Each phase repeats the overhead sweep. `summary.json` records events/s per type, so a noisy type is visible at once |
| Dump storms / disk | Cooldown, per-session caps, retention |
| Recorder silently inert (P9) | Hook heartbeat: every registered hook increments a per-hook "fired" bit; `session.json` and every dump list hooks that never fired, and the first dump warns |
| `framebufferOnly = NO` cost | Only when the ring or a screen capture is enabled; measured in phase 2 |
| Prebuilt runtime limits what can be hooked | Everything above is in the executable or the Metal plugin; runtime-only items (host audio queue, SDK lock waits) come from host stacks or wait for a runtime rebuild |
| Executable symbol export to the plugin | Check with `nm`; reverse direction as a fallback |
| Privacy for the public bug report | Paths stripped, never auto-uploaded (phase 5) |

## 14. What was verified and what is inferred

**Verified in code or data:**

- the frame CSV is on by default, with its caps and writer;
- the lr sampler reads other threads' `PPCContext` without suspending them;
- generated code keeps registers in `ctx` and stores `lr` before every call;
- the guest stack walk guards;
- the `SafeRead` helpers;
- the 30 s, 6000 and 50 limits;
- the blocking `press`;
- guest-only captures and no `framebufferOnly` setting;
- `std::_Exit` on close;
- the log rotation settings;
- the SDK AV log text and where it is emitted;
- the exported `AddSink`, `OnWindowCloseRequested`, `OnGuestThreadExit`;
- Tracy compiled out in Release;
- the import names in the generated code;
- the input boundary hook;
- function names carrying their addresses;
- the event rates of traced and untraced runs;
- the pain points as quoted from the docs.

**Inferred, to check in phase 1–3:**

- the executable exports `fable2_fr_api` to the plugin;
- `ctx.r1` is current at call boundaries for cross-thread reads;
- the guest KTHREAD stack range is readable at +0x5C/+0x60;
- the host-stack suspend time;
- the sampler cost of about 0.1% of a core;
- the completed-handler and drawable-readback costs;
- test `close` reaching `OnWindowCloseRequested`;
- region paths in `NtCreateFile` marking area changes;
- Fable setting rich-presence contexts;
- the audio frame period;
- the guest allocation import names;
- where the Lua error path is.

## 15. Implemented in phase 1 (2026-10-08)

Phase 1 is built and validated in background test runs (Cullis Gate save, ABBA overhead sweeps). The
recorder is **on by default**; `FABLE2_TRACE=off` turns all of it off (no thread, no sink, no folder).
Everything else in phase 1 is cheap enough to stay on; the heavy parts (detail levels, streaming) are
opt-in through the switch.

### 15.1 What exists

| Part | Where | Notes |
|---|---|---|
| Recorder core | `src/diagnostics/fable2_flight_recorder.{h,cpp}` (executable) | Per-thread single-producer rings of 32-byte events (8k events; 32k for GameThread / 3D Engine / GPU Commands / Audio Worker / Main; 256k for the recorder's own sampler ring), one global multi-producer ring for log lines (signal-safe, validated by position), registry of 250 rings, built-in + dynamic event types, game-state tags, hook heartbeat, triggers, dump writer, `session.json`, `log-digest.jsonl`, optional `stream.f2t`. Mach-tick time base with clock pairs (mach / CLOCK_MONOTONIC / uptime-raw / realtime) in every header; every event carries the guest frame id |
| C API to the plugin | `src/diagnostics/fable2_trace_api.h`, `fable2_fr_api()` | **Verified:** the executable exports it (`nm -gU fable_2` → `T _fable2_fr_api`; `dyld_info -exports`); the plugin finds it with `dlsym(RTLD_DEFAULT, …)` ("Metal plugin: flight recorder API found" in every log). The reverse direction was not needed |
| Plugin client | `backends/metal/port/rex/graphics/metal/metal_trace.{h,cc}` | Swap (frame id + gap/hang detector), paint, frame-CSV bridge (every `frame_log` row also becomes a ring event, independent of the CSV cvar; CSV and recorder share frame ids) |
| Shared guest walker | `src/diagnostics/fable2_guest_stack.h` | One guarded back-chain walk for `skipintro::guest_stack`, `InBootLoader` and the recorder. The build test checks the new `InBootLoader` against the old loop on 20,000 random chains: identical decisions. Cross-thread walks use the thread's PCR stack range (`+0x70` base / `+0x74` end) and `mach_vm_read_overwrite` windows, because the SDK frees a guest stack inside `XThread::Exit` before `running_` drops |
| All-thread sampler | recorder thread, 100 Hz (`sampler:3` = 1 kHz) | Every running guest XThread: `lr`/`r1` read without suspension, 14-frame chain (re-walked only when `r1` changes), state WAIT (inside a hooked kernel wait) / SPIN (known poll loops) / WORK; unchanged samples are folded into `sample_repeat` counts. Measured **0.07 % of one core** in an uncapped sweep (§15.7) — it stays always-on |
| Hooks on the recorder | `src/core/fable2_loading_pacing.h`, `fable2_skip_intro.h`, `fable2_turbo.h`, `fable_2_app.h` | Kernel waits/sleeps (≥ 2 ms → event with 4-frame stack, ≥ 50 ms → 14 frames, ≥ 1 s → `long_wait`), the guest wait wrapper (handle/result), `DelayMilliseconds` ≥ 2 ms, poll-wait ≥ 50 ms, set-event while a poll-wait is open > 300 ms, worker-0 ops, the level-load call, save-notice sleep, `XamContentClose`, every file open (path), `NtClose` (movie handle), `NtReadFile` at `io:2` |
| Log sink | `FrSink` (`rex::AddSink`) | INFO+ lines into the log ring; WARN/ERROR into a fixed 2048-entry lock-free digest (first occurrence kept in full with tags, then counted; templates strip numbers/hex), flushed to `log-digest.jsonl` every second and at every dump; trigger patterns below. Lines that start with `[trace]` are ignored by the AV matcher |
| Test control | `metal_test_control.mm`, `tools/fable2_test_control.py` | `press` runs on its own thread (reply when released); `down`/`up`; `trace dump [reason]`, `trace mark <text>`, `trace tag k=v`, `state`; `capture screen` (drawable after ImGui); `close` writes the `exit` dump first |
| Overlay capture | `MetalPresenter::CaptureScreen` | `framebufferOnly = NO` only while a request is pending (restored on the next paint), blit after `endEncoding`, completion handler wakes the caller. Measured 85–107 ms per capture; zero cost otherwise |
| Launch parameters | `tools/fable2_dev_session.py` | `--game-arg=…` (repeatable) and `--cache-from DIR`, **test sessions only** (normal launches refuse them). Writes `trace/tags-in.txt` (LevelName/QuestName of the booted save, and of every save the watcher copies) inside the session folder only |
| Dev Launcher | `tools/macos/DevLauncher/main.swift` | "Diagnostics": trace preset (default / off / loading / GPU / everything) and a free-text "Extra env" field (`NAME=VALUE …`); CLI `--trace-preset`, `--extra-env` |
| Reader + harness | `tools/f2diag/`, CLI `tools/f2diag.py` | §15.6 |
| Build test | `tests/flight_recorder_tests.cpp` (POST_BUILD, like the config test) | walker equivalence, switch + aliases, records, synchronous dump, first-AV dump + digest, emit cost (8–10 ns/event) |

### 15.2 Switch reference

`FABLE2_TRACE=<item>[,<item>…]`, parsed once in `Fable2App::OnPostInitLogging` before any guest thread.

| Item | Meaning |
|---|---|
| (unset) | Defaults: every category at level 1 (ring) except `tex` (0). Recorder, sampler, triggers, digest on |
| `off` | Everything off: no recorder thread, no log sink, no `trace/` folder (overhead A/B) |
| `cat:N` | Category level: 0 off, 1 ring, 2 also stream to `trace/stream.f2t`, 3 detail (old log lines) |
| `all:N` | Every category |
| `gap=<ms>`, `hang=<ms>` | Frame-gap (default 1000) and hang (default 3000) thresholds |
| `nohost`, `noring` | Accepted, no effect yet (host stacks are phase 2) |

Categories: `frame gpu cpu sampler wait signal io worker0 load ui shader draw clock input log mark tag crash tex shmem save`.

Environment variables next to it: `FABLE2_TRACE_GAP_MS`, `FABLE2_TRACE_HANG_MS`, `FABLE2_TRACE_SAMPLER_HZ`
(0 = sampler off), `FABLE2_TRACE_MAX_DUMPS` (20), `FABLE2_TRACE_LOG_SECONDS` (window for the old log lines;
default unlimited), `FABLE2_TRACE_DIR` (override the folder), `FABLE2_TRACE_SELFTEST=gap[@s],av[@s]`
(validation only: block the swap thread once for 1.5 s, or log one synthetic AV line, at s seconds after
the first frame; defaults 30 s and 20 s).

Aliases (each logged at startup as `aliases: X=1 -> …`):

| Old switch | Expands to | Change in behaviour |
|---|---|---|
| `FABLE2_LOADING_TRACE=1` | `load:3,wait:3,io:2,worker0:2,signal:1,cpu:2,sampler:3` | All `[loading-pacing]` lines kept, **no 30/60 s windows, no 6000/300/500/50 caps** (`FABLE2_TRACE_LOG_SECONDS` limits them if wanted); `[stack-sample]` lines now from the all-thread sampler |
| `FABLE2_STACK_SAMPLE=1` | `sampler:3` | 1 kHz all-thread sampler; `[stack-sample] t=… n=… | count thread lr <caller…` every second (every running or polling guest thread; blocked WAIT samples excluded — no longer only callers of two helpers). The helper-entry sampler (`fable2_stack_sample.h`, also hand-added to `YieldAndCheckThreshold_82CBD098.cpp`) is removed |
| `FABLE2_GPU_TIME_TRACE=1` | `gpu:3,cpu:2,sampler:3,frame:2` | `smp_*` CSV rows and the 10 s `[gpu-time-trace] guest lr top` line now come from the recorder's sampler (the plugin's `GuestSampler` thread is gone); `gpu_cb`, `tcpu_*`, `tr_*` rows unchanged |
| `FABLE2_DRAW_SKIP_TRACE=1` | `draw:2,tex:3,shmem:2` | CSV unchanged; ring copies of `ds_*` / `tr_tex_*` / `tr_n_*` rows |
| `FABLE2_SHMEM_CHECK=1`, `FABLE2_CRASH_DIAG=1` | `shmem:3`, `crash:3` | unchanged |
| `FABLE2_SKIP_INTRO_TRACE=1`, `FABLE2_SAVE_TRACE=1`, `FABLE2_BOOT_SAVE_TRACE=1`, `FABLE2_CLOCK_REPORT` | `io:3`, `save:3`, `load:3`, `clock:3` | log lines kept |

### 15.3 Files per session

```
<state>/logs/trace/          (dev/test sessions: out/dev-sessions/<id>/test-state/logs/trace;
                              a shared log folder keeps one previous run as trace-prev/)
  session.json               build (git describe, binary sha256), env, modified cvars, Mac model/cores/RAM,
                             clock pairs, levels, aliases, tags, hooks fired / never fired, dumps per reason
  log-digest.jsonl           one row per WARN/ERROR template: level, count, first {t_s, frame, tid, line, tags}
  tags-in.txt                level_name / quest pushed by tools/fable2_dev_session.py (polled every second)
  stream.f2t                 only when a category is at level 2+
  dumps/NNN-<reason>/
    ring.f2t                 "F2T1" + JSON header (types, threads, guest threads, host threads, tags, strings,
                             hooks, clocks) + every ring's valid records
    summary.json             trigger, detail, tags, last 120 frame intervals, all-thread stacks (3 snapshots
                             + common outer frames), hooks, recorder self-report (events/s, thread CPU %)
    stacks.txt               the same stacks as text (raw addresses; f2diag symbolises)
```

Triggers and limits: `frame_gap` (no swap for > gap ms, not inside the level-load call; cooldown 10 s),
`hang` (no guest swap for 3 s, again at 10 s and 30 s; presenter paint no longer masks it, see `boot-hang-20261008.md` §5), `first_av` (once), `av_storm`
(> 100 AV lines in a second; 60 s cooldown, max 3), `gpu_timeout` (`GPU timeout` / `GPU sync timeout`
lines; 30 s, max 5), `shader_miss` (`persistent miss`; 60 s, max 3), `crash_diag` (`[crash-diag]`),
`pipeline_fail` (`Failed to create pipeline state` / `materializeAll`), `user[-x]` (test control, no
limit), `exit` (window close or test-control close, once). Per session at most 20 automatic dumps / 200 MB.
Frame-gap and hang detection pause while a normal session is unfocused or minimised (background test mode
keeps them). Dumps measured 1–2.5 MB.

### 15.4 Game-state tags v1

| Tag | Source (as built) |
|---|---|
| `level_load`, `loading` | `sub_825F3050` enter/exit; `loading` = level-load count, cleared after 60 swaps in a row < 100 ms |
| `save_notice` | count of "Saving content" minimum-display sleeps (`DelayZero_82CBC6B0`, return `824EC440`) |
| `region` | **Changed from the design.** The game opens no per-level files during an area transition (a probe run logged every open: only `save:\…`, `loadingscreendata.txt`, `locomotion.loco`, banks at boot). The recorder thread therefore scans the loading thread's stack (and one pointer level) for a name from `levelconnectiongraph.txt` when the level-load call starts. A save boot names its level (`*(sp+0x314)` = `Caves\BowerLake\ThagsCave`); the Cullis Gate transition does not, so the tag reads `unknown` then. The loading-screen thread (3D Engine, `loadingscreendata.txt`) holds the whole region table, so it cannot tell the destination either |
| `level_name`, `quest` | `tags-in.txt` from the session watcher: the booted save at launch and every save it copies (an F5 in Bower Lake set `level_name=BowerLake`, `quest=QC070_Thag`) |
| `movie` | `.bik` open → file name, cleared when that handle is closed |
| `turbo`, `fps_mode`, `focus` | `turbo::SetActive`, `fps::Use60()`, window focus/minimise |
| `selftest` | which self-test fired |

### 15.5 Line references re-verified (code moved after the design)

`IssueSwap` `metal_command_processor.cc:1984`; GPU timeout lines `:1619`, `:1648`; `gpu_time_trace` `:156`
(the lr `GuestSampler` that was at l.212–312 is replaced by `GuestSamplerRows`, `:231`); `DrawSkipTrace`
`:2682`; persistent miss `metal_stage_compile_cache.cc:261`; `PaintAndPresentImpl` `metal_presenter.mm:471`.
The save-notice sleep is `DelayZero_82CBC6B0` (return `824EC440`), not `DelayMilliseconds`. `OnGuestThreadExit`
fires only for the main thread in this SDK (`rex_app.cpp:473`), so it cannot mark exiting threads; the
sampler drops non-running threads at its 1 s refresh instead.

### 15.6 `tools/f2diag` v1 and the old scripts

`python3 tools/f2diag.py <cmd> <session|trace|dump>`: `report` (trigger, tags, recorder self-report, silent
hooks, symbolised stacks, top sampler stacks 2 s before, timeline, digest rows near it), `dumps`, `timeline
[--from --to --fps]`, `gaps [--threshold]`, `threads` (WORK/SPIN/WAIT shares, host CPU ms, waits by call
site), `stacks [--thread]`, `phases`, `digest [--grep]`, `export [--jsonl]` (Perfetto/Chrome JSON for
ui.perfetto.dev), `sym ADDR…`, `images dark|blue <result.json>`, `run sweep|area|move|f5|boot|bootloop <label>`.
Modules: `f2t.py` (reader), `symbols.py` (from `generated/default/fable_2_register.cpp`, cached in
`~/.cache/f2diag`), `timeline.py`, `report.py`, `perfetto.py`, `session.py`, `pacing.py`, `images.py`,
`threadcpu.py`, `run.py`. Pure Python (numpy and PIL are not installed for the system python3).

Run verdicts (2026-10-09): `run.exit_check()` adds `exit_reasons` (`exited_early` = pid gone before the harness close,
checked with `kill(pid, 0)` at each capture and before close; `crashed` = a `fable_2-*.ips` report in
`~/Library/Logs/DiagnosticReports` for the pid newer than `start_unix`, read only, with the faulting thread's top frames;
missing `Title terminated; hard-exiting process.` after the harness asked to close). Any reason fails the run, as AV/FATAL
lines already did. `f2diag run verdict <session>` recomputes the verdict of a past session. This covers the abort gap
until phase 2 writes a synchronous crash dump (a Metal `MTLReportFailure` abort leaves no recorder dump).

| Old script (out/validation/…) | Now |
|---|---|
| `flash-20261007/sweep.py`, `cr-fixes-20261007/sweep.py`, `playtest-20261005/sweep.py` (+ their `subprocess.run` / `prepare_session` monkeypatches) | `f2diag run sweep <label> [--sweep-plan default\|fast\|turn30] [--fps] [--game-arg=…] [--cache-from DIR] [--cpu]`; `run.camera_sweep()` |
| `flash-20261007/dark.py` | `f2diag images dark result.json`; `images.dark_dips()` (contact sheets: `images.to_png()` via sips) |
| `flash-20261007/analyze.py`, `playtest-20261005/blue.py`, `flash-20261007/blue.py` | `f2diag images blue result.json`; `images.blue_spikes()` |
| `load-profile-20261006/boot.py`, `cr-fixes-20261007/boot.py`, `lp_analyze.py`, `phases.py`, `white-phase-20261005/val.py` | `f2diag run boot <label>`; load phases from the trace: `f2diag phases`, `f2diag timeline --fps` (level-load begin/end, loading tag, gaps); no pixel-mean constants or hard-coded clock offsets |
| `flash-20261007/movecheck.py` | `f2diag run move <label>`; `images.moved()` |
| `cr-fixes-20261007/f5save.py` | `f2diag run f5 <label>` |
| `area-load-20261008/run.py`, `press.py` | `f2diag run area <label> [--press-at --hold --max]`; presses: `Run.press()` (non-blocking) or `fable2_test_control.py press --no-wait` |
| `area-load-20261008/frames.py`, `logt.py`, `phasecpu.py`, `flash-20261007/health.py`, `cr-fixes-20261007/sweepstats.py` | `f2diag timeline --fps`, `f2diag gaps`, `f2diag threads`; `pacing.window()` |
| `cr-fixes-20261007/threadcpu.py` | `tools/f2diag/threadcpu.py` (moved as is) |
| `/usr/bin/sample` share scripts (`sshare.py`, `callers.py`, `sampletree.py`, `prof.py`) | `f2diag stacks` / `f2diag threads` on the always-on sampler (no task suspension) |
| `flash-20261007/dumplib.py`, `diffdraws.py` (draw dumps) | not moved yet (phase 4, with the draw-record ring) |
| `weak-mac-perf-20261008/gpustats.py`, `passstats.py` | `f2diag gpu <session>` (`tools/f2diag/gpu.py`, 2026-10-08): GPU ms/frame, GPU busy % and idle gaps, GPU Commands blocked shares, `tr_n_*` counters (incl. `tr_n_upl_*` staged uploads and `tr_n_tex_*` texture-load flushes/loads/pre-commit, step 7/8 of `docs/weak-mac-perf-20261008.md`), per-pass time, main-pass breaks by cause (`texture:*` end details = texture_upload group); trace-only data checks `FABLE2_UPLOAD_VERIFY=1`, `FABLE2_TEXTURE_PRECOMMIT_VERIFY=1` |

### 15.7 Validation (2026-10-08, background test mode, each run closed and `pgrep -x fable_2` = 1)

Runs live under `out/dev-sessions/20261008-04*` (ignored; the maintainer may delete `out/`). Every run was a
background test session (`--test-control`, target slot none) on save `hero1-dog-live_2026-10-07_18-49-47`.
Each rebuild's first launch is counted, none discarded.

**Mechanism runs (m1–m5).** Untraced defaults plus `FABLE2_TRACE_SELFTEST=gap@40,av@20`. Each run held A
through the Cullis Gate (Thag's Cave → Bower Lake) and exercised test control mid-run; m4 and m5 also pressed F5 in Bower Lake.
- m1 found two bugs, both fixed:
  - a stale clock in the detector made `since` underflow, which wrote spurious `frame_gap`/`hang` dumps;
  - the recorder's own "dump written" line counted as a second AV.
- m2–m5 are clean. m5 (`20261008-043336-4fc30e6d`) wrote exactly `001-first_av`, `002-frame_gap`,
  `003-user-probe` and `004-exit`, at 1.5–2.5 MB each:
  - **`first_av`** at 20.7 s holds the synthetic line, and `log-digest.jsonl` shows the AV row with count 1.
  - **`frame_gap`** at 41.7 s: "no guest swap for 1030 ms". The 3 snapshots of all 18 guest threads show
    the 3D Engine SPIN in the ring poll (`ProcessAndProcessAndProcess1087_82242628 < MainRenderLoop_82B9CD68 <
    … < Thread_82CCA428`) while GameThread is in `ProcessGameFrame`, exactly what a stalled GPU Commands
    thread produces. The sampler histogram of the 2 s before agrees: 3D Engine 87–88 % SPIN at that site.
  - **Tags** (from `f2diag phases`):
    - 2.99 s: level-load #1 (405 ms). The region is `Caves\BowerLake\ThagsCave` and `level_name` comes from the booted save.
    - 11.17 s: save notice (the transition autosave).
    - 11.89 s: level-load #2 (715 ms). The region is `unknown`, because the transition doesn't name its destination on the stack (§15.4).
    - 13.0 s: the `trace mark`.
    - 42.2 s: the 1509 ms gap.
    - 45.97 s: save notice (F5).
    - 49.8 s: `level_name = BowerLake` and `quest = QC070_Thag`, pushed by the watcher after the F5 save.
  - **Heartbeat:** all 20 registered hooks fired, and the "never fired" list is empty in `session.json` and in every dump.

**Overhead (ABBA, 30 s steady camera turn, `f2diag run sweep --sweep-plan turn30 --no-capture --cpu`).**

| Run | FPS | 1 % low | p50 / p95 / p99 ms | recorder thread (% of one core) | process CPU |
|---|---|---|---|---|---|
| uncapped, default (on) | 154.93 | 85.1 | 5.57 / 10.83 / 11.46 | 0.07 | 264.9 % |
| uncapped, `FABLE2_TRACE=off` | 154.10 | 85.4 | 5.61 / 10.80 / 11.40 | — | 264.9 % |
| 60, `FABLE2_TRACE=off` | 60.00 | 54.1 | 16.62 / 17.94 / 18.23 | — | 179.2 % |
| 60, default (on) | 60.00 | 53.7 | 16.64 / 17.97 / 18.43 | 0.06 | 177.6 % |

Uncapped FPS is +0.5 % with the recorder on, so the two arms are equal within noise (budget ±1 %). The 60 FPS
p95 is unchanged; p99 differs by 0.2 ms (max 19.1 vs 18.9 ms), within run-to-run noise. Over a whole session the
recorder thread uses 0.19–0.22 % of a core, including the start-up sha256 of the binary and the exit dump. The
recorder logs ~1.9–2.4 k events/s at 8–10 ns per emit (build test), which is negligible next to the 100 Hz sampler.

**Test control.**
- In m2–m5, a `press w 3000` ran while these commands completed during the hold: two `capture`s (40–56 ms each),
  a `capture screen` (85 ms, includes the ImGui overlay) and a `trace dump` (30 ms).
- `press f7` turbo toggles, `state`, `trace mark` text and the `user-probe` dump name all work.
- `close` writes the `exit` dump first.

**Aliases.** One run with `FABLE2_LOADING_TRACE=1 FABLE2_STACK_SAMPLE=1 FABLE2_GPU_TIME_TRACE=1` (`alias2`):
- 5,597 `[loading-pacing]` lines in 28 s, with no window or cap;
- `[stack-sample]` lines every second;
- `[gpu-time-trace] guest lr top` lines every 10 s, with the 3D Engine 67–85 % in `822426E8*`;
- `smp_*` CSV rows;
- `stream.f2t` at 19 MB in 28 s, read by f2diag with thread names.

**Normal-play safety.**
- `python3 -m unittest` passes for `tests.profile_options_tests`, `checkpoint_control_tests`,
  `test_window_position_tests`, `session_replay_tests`, `app_test_bridge_tests` and the new
  `dev_session_trace_options_tests` (9 tests). These include that a normal launch's `open` command has no game
  arguments and no cache seeding, and that `--game-arg` / `--cache-from` are refused outside test sessions.
- `screenshot_timestamp_tests` fails the same way before and after this work (its fixture has no `session.json`).
- The live state folder (`~/Library/Application Support/Fable II Recomp`, 92 files) had identical sizes and
  mtimes before m5 and after the last run.

**Final checks.**
- 0 real guest AVs in all 13 sessions; the only AV lines are the self-test's `[trace-selftest]` lines.
- F5 quick save worked in m4 and m5 (the watcher copied a `BowerLake` / `QC070_Thag` save).
- Movement works (`f2diag run move mv1`): 80 % of the view changed after walking, and the capture shows the
  hero at the Cullis Gate prompt.
- `pgrep -x fable_2` returned 1 after every run.

### 15.8 Deferred to phase 2+

- Host stacks at a trigger (suspend/resume, `nohost`), fatal-signal ring write (`crash.f2t`), `slot_wait`,
  `wait_timeout`, `draw_skip_burst`, early `gpu_timeout` from command-buffer completion, `long_load`.
- `gpu` commit/complete events always on, `shader` events, per-frame `draw` skip counts, `FR_COUNT` counters.
- Frame ring of downscaled presented frames, F9 bug report, `capture ring`, PNG writing in the game.
- `wait-for` as a game-side command (it exists in Python, polling `state`), MCP `dump`/`state`/`timeline` tools.
- A reliable destination region at area transitions (needs RE of the level manager's current-level field).
- `ui` tag (UI signature names), `rich_presence`, `input`/`audio`/`mem`/`net`/`script` categories (phase 3).
- Moving the draw-dump tools into f2diag (phase 4).
- **Synchronous crash dump (gap found 2026-10-09).** The `first_av` dump is written later by the recorder thread
  (`HandleTriggers` polls `trig_flag`). When an abort follows within milliseconds, it is lost. Session
  `20261009-144547-b8c73b0d` (`FABLE2_GUEST_BARRIERS=0`) is an example: first AV at 49.905, `[FATAL] Call to invalid or
  unregistered function` at 49.914, then SIGABRT, leaving an empty `dumps/` folder. Proposal: a `[FATAL]` hook in the
  log sink, plus a SIGABRT / `std::terminate` handler, that writes a minimal dump synchronously on the crashing
  thread (log ring, tags, AV count, first AV line, thread names) before the process dies. This is the same family as
  the `crash.f2t` fatal-signal ring write above. Not implemented.
