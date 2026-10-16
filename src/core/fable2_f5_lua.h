// fable_2 - Host hotkeys that run external Lua files in the in-game Lua state,
// using the game's own CScriptManager::RunScript (C++ core API).
//
//   F5 (or number-row 5): quick save. Native (src/core/fable2_quick_save.h): the
//       same save as the pause menu's "Save and Continue". If `f5_lua_path` is set,
//       F5 runs that Lua file instead.
//   F6 (or number-row 6): custom Lua. Runs `f6_lua_path` (default
//       scripts/recomp/F6.lua, the script selector menu).
// Each key runs its file exactly the way the Lua "RunScript(path)" global does,
// but from the host side.
//
// How it works (all hooks are recompiled-function probes so they have ctx+base):
//   A) poll_f5() is called per frame from the host input poll; it edge-detects
//      F5 and latches a flag (g_f5_pending).
//   B) poll_mainloop() is called per frame from the MainRenderLoop hook; when
//      the flag is latched it calls run_external().
//   C) run_external() builds a game (refcounted) string for the path in guest
//      memory and calls the captured CScriptManager::RunScript method.
//   D) The dispatcher probe (LuaBind_RunScript_82806168) captures the
//      (this, method) of the RunScript callable: the binding dispatches to a
//      member fn, so the wrapper holds (this=instance, method=fn). We pin the
//      (this, method) whose string argument ends in ".lua".
//
// The RunScript binding and the MainRenderLoop hook both run on the guest main
// thread (the thread that drives the in-game Lua VM), so replaying the run is
// thread-safe.

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "rex/cvar.h"
#include "fable2_character_editor.h"  // opt-in mod: F6 entry + save backup ack
#include "rex/ppc/context.h"  // PPCContext

// Guest-memory helpers (defined in src/diagnostics/fable2_text_append.h, which
// is included later in main.cpp). We only need these five; forward-declare them
// here so this header can be included before fable2_text_append.h. The inline
// definitions in fable2_text_append.h satisfy these declarations.
namespace fable2::textappend {
inline uint64_t host_addr(const uint8_t* base, uint32_t a);
inline bool rdable2(const uint8_t* base, uint32_t a, size_t n);
inline bool wr2(const uint8_t* base, uint32_t a, size_t n);
inline uint32_t load_be(const uint8_t* base, uint32_t a);
inline uint32_t guest_alloc(PPCContext& ctx, uint8_t* base, size_t size);
}

#ifdef _WIN32
#include <windows.h>
#include <share.h>
#define FABLE2_F5_VK 0x74  // F5: quick save
#define FABLE2_F6_VK 0x75  // F6: custom Lua
#endif

// External Lua files run by the host hotkeys. Paths are resolved by
// CScriptManager::RunScript relative to the VFS root (data/) and must be listed
// in data/dir.manifest.
REXCVAR_DEFINE_STRING(
    f5_lua_path,
    "",
    "Input",
    "Optional Lua file run by F5 / number-row 5 (host keyboard) INSTEAD of the "
    "native quick save. Empty (default) = quick save. Path relative to the VFS "
    "root (data/), as CScriptManager::RunScript / loadfile resolves it.");
REXCVAR_DEFINE_STRING(
    f6_lua_path,
    "scripts/recomp/F6.lua",
    "Input",
    "Custom Lua file run by F6 / number-row 6 (host keyboard). Path relative to "
    "the VFS root (data/). The default F6.lua opens the script selector menu.");

// Current game-state name (defined in src/diagnostics/fable2_state_probe.h,
// which is included later in main.cpp). Logged before each RunScript call so
// a crash is self-explanatory (front-end vs. gameplay vs. cutscene). Must be
// declared at top level (NOT inside fable2::f5lua - nested namespace
// declarations nest, which would create fable2::f5lua::fable2::stateprobe).
namespace fable2::stateprobe {
const char* CurrentStateName();
}

// Native quick save (src/core/fable2_quick_save.h, included later in main.cpp).
namespace fable2::quicksave {
inline void request();
}

namespace fable2::f5lua {

// Guest-memory helpers live in fable2::textappend (src/diagnostics/fable2_text_append.h).
namespace ta = fable2::textappend;

// The captured CScriptManager "run file" callable (guest addresses).
// The FIRST .lua dispatch (boot: 'miscellaneous/GeneralScriptManager.lua' via
// the canonical file-runner method) is pinned; later same-method dispatches
// refresh `this` in case the manager instance moves (a stale instance
// null-dereferences guest memory - the "read of guest 0x0" access violation).
// Dispatches of OTHER methods that also take a .lua path are ignored.
inline std::atomic<uint32_t> g_this{0};   // CScriptManager instance
inline std::atomic<uint32_t> g_method{0}; // member fn (CScriptManager::RunScript)
// this[0] as read at capture time (vtable-like; stable for the object's
// lifetime). A freed/reused block almost always differs, which catches the
// stale-`this` case even when the new first word happens to be a plausible
// pointer.
inline std::atomic<uint32_t> g_expected_head{0};

// Monotonic F5 run counter: the crash log (fable2_f5_lua.log is flushed per
// line) always shows the last run number before a hard crash.
inline std::atomic<uint32_t> g_run_count{0};

// Latched by the hotkeys; consumed by poll_mainloop.
inline std::atomic<bool> g_f5_pending{false};  // quick save (f5_lua_path)
inline std::atomic<bool> g_f6_pending{false};  // custom Lua (f6_lua_path)
inline bool g_f5_down_prev = false;
inline bool g_f6_down_prev = false;

// Generic host -> guest script request (any thread). The next MainRenderLoop
// frame runs it through the same RunScript path as F6 (guest main thread).
// Used by the in-game message bridge (src/core/fable2_game_message.h). One slot:
// a newer request replaces an unconsumed older one.
inline std::mutex& script_mtx() { static std::mutex m; return m; }
inline std::string g_script_pending;          // guarded by script_mtx()
inline std::atomic<bool> g_script_has{false};
// Result of the last generic request: 0 none yet, else sequence number.
inline std::atomic<uint32_t> g_script_req_seq{0};   // bumped by request_script
inline std::atomic<uint32_t> g_script_ran_seq{0};   // request consumed, RunScript called
inline std::atomic<uint32_t> g_script_fail_seq{0};  // request consumed, could not run

inline uint32_t request_script(const std::string& path) {
  std::lock_guard<std::mutex> l(script_mtx());
  g_script_pending = path;
  const uint32_t seq = g_script_req_seq.fetch_add(1) + 1;
  g_script_has.store(true);
  return seq;
}

// Acknowledgements from guest Lua: a script signals the host by calling
// RunScript("<ack path>") on a no-op file; the RunScript dispatcher probe below
// sees the path and bumps the matching counter. See src/lua/recomp_notify*.lua.
inline std::atomic<uint32_t> g_notify_shown{0};
inline std::atomic<uint32_t> g_notify_declined{0};

inline std::mutex& log_mtx() { static std::mutex m; return m; }
inline std::FILE* logf() {
  static std::FILE* f = [] {
#if defined(_WIN32)
    // Open with shared read/write so the log can be read live while the game
    // holds it open (plain fopen() takes an exclusive lock on Windows).
    std::FILE* out = ::_fsopen("fable2_f5_lua.log", "a", _SH_DENYNO);
    return out;
#else
    return std::fopen("fable2_f5_lua.log", "a");
#endif
  }();
  return f;
}
inline void logline(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> l(log_mtx());
  if (std::FILE* f = logf()) {
    std::fputs(buf, f);
    std::fflush(f);
  }
}

// Mac window events and Windows polling both queue work for the guest thread.
// key 5 = quick save (f5_lua_path), key 6 = custom Lua (f6_lua_path).
inline void request_run(int key = 5) {
  if (key == 5 && std::string(REXCVAR_GET(f5_lua_path)).empty()) {
    fable2::quicksave::request();
    return;
  }
  (key == 6 ? g_f6_pending : g_f5_pending).store(true);
}

// Per-frame F5/F6 edge detector. Call from the host input poll (per frame).
inline void poll_f5() {
#ifdef _WIN32
  const bool down5 = (GetAsyncKeyState(FABLE2_F5_VK) & 0x8000) != 0;
  const bool down6 = (GetAsyncKeyState(FABLE2_F6_VK) & 0x8000) != 0;
  if (down5 && !g_f5_down_prev) request_run(5);
  if (down6 && !g_f6_down_prev) request_run(6);
  g_f5_down_prev = down5;
  g_f6_down_prev = down6;
#endif
}

// Run the external Lua file via the captured CScriptManager::RunScript.
// Returns true if invoked. Must be called from the guest main thread.
inline bool run_external(PPCContext& ctx, uint8_t* base, const std::string& path) {
  const uint32_t this_ptr = g_this.load();
  const uint32_t method = g_method.load();
  if (this_ptr == 0 || method == 0) {
    logline("[f5] requested but RunScript callable not captured yet\n");
    return false;
  }
  if (path.empty()) return false;

  // Safety: the captured method must resolve to a real recompiled guest
  // function, otherwise REX_CALL_INDIRECT_FUNC will jump to a bad pointer.
  if (method < 0x82000000 || method > 0x82FFFFFF ||
      !rex::runtime::ResolveIndirectFunction(method)) {
    logline("[f5] method=0x%08X does not resolve; not calling\n", method);
    return false;
  }

  // Safety: the captured CScriptManager instance must still be alive. RunScript
  // reads *(this) first and calls through it, so a freed/recreated manager
  // (head == 0, 0xFFFFFFFF, 0xCD.. free-fill, or an otherwise implausible
  // pointer) null-dereferences downstream. Skip this run; the next .lua
  // dispatch refreshes the capture. (Log line is flushed, so the run counter
  // below proves the guard fired before any crash.)
  if (!ta::rdable2(base, this_ptr, 8)) {
    logline("[f5] this=0x%08X no longer readable; skipping (capture refreshes)\n",
            this_ptr);
    return false;
  }
  const uint32_t head = ta::load_be(base, this_ptr);
  const bool head_plausible =
      (head >= 0x1000 && head < 0x82000000 && ta::rdable2(base, head, 4)) ||
      (head >= 0x82000000 && head <= 0x82FFFFFF);
  if (head == 0 || head == 0xFFFFFFFF || !head_plausible) {
    logline("[f5] this=0x%08X head=0x%08X not a live manager; skipping\n", this_ptr,
            head);
    return false;
  }
  const uint32_t expected_head = g_expected_head.load();
  if (expected_head != 0 && head != expected_head) {
    logline("[f5] this=0x%08X head=0x%08X != expected 0x%08X (manager moved or "
            "freed); skipping\n",
            this_ptr, head, expected_head);
    return false;
  }

  const uint32_t run_n = g_run_count.fetch_add(1) + 1;
  logline("[f5] run #%u state='%s' path='%s'\n", run_n,
          fable2::stateprobe::CurrentStateName(), path.c_str());

  // 1) Source C-string in guest memory.
  const uint32_t src = ta::guest_alloc(ctx, base, path.size() + 1);
  if (!src || !ta::wr2(base, src, path.size() + 1)) {
    logline("[f5] src alloc failed src=0x%08X\n", src);
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(ta::host_addr(base, src)), path.data(), path.size());
  *reinterpret_cast<char*>(ta::host_addr(base, src + path.size())) = '\0';

  // 2) String object (the game's refcounted String).
  const uint32_t str_obj = ta::guest_alloc(ctx, base, 64);
  if (!str_obj || !ta::wr2(base, str_obj, 64)) {
    logline("[f5] str_obj alloc failed str_obj=0x%08X\n", str_obj);
    return false;
  }
  for (uint32_t i = 0; i < 64; i += 4) {
    uint32_t z = 0;
    std::memcpy(reinterpret_cast<void*>(ta::host_addr(base, str_obj + i)), &z, 4);
  }

  // DIAG: trace each guest step so fable2_f5_lua.log shows the last step before
  // a hard crash (the log is flushed per line).
  logline("[f5] step1 src=0x%08X str_obj=0x%08X this=0x%08X head=0x%08X method=0x%08X "
          "-> ConstructRefCounted\n",
          src, str_obj, this_ptr, head, method);
  // ConstructRefCounted_8222CF18(dest, cstring, len=-1): build the game string.
  ctx.r3.u32 = str_obj;
  ctx.r4.u32 = src;
  ctx.r5.u32 = 0xFFFFFFFF;
  REX_CALL_INDIRECT_FUNC(0x8222CF18);
  logline("[f5] step2 after ConstructRefCounted -> RunScript\n");

  // CScriptManager::RunScript(this, const String& path).
  ctx.r3.u32 = this_ptr;
  ctx.r4.u32 = str_obj;
  REX_CALL_INDIRECT_FUNC(method);
  logline("[f5] step3 after RunScript -> (keeping string alive) done\n");

  // NOTE: we deliberately do NOT call DestructRefCounted on str_obj here.
  // CScriptManager::RunScript may defer the actual file load/compile to a later
  // frame; freeing the path String synchronously would be a use-after-free the
  // moment the deferred load dereferences it (observed as a "read of guest 0x5"
  // access violation, intermittent, only when the menu is up). Leaving the
  // string's refcount at 1 keeps it valid for the deferred load. The small
  // per-press leak is acceptable for an on-demand utility.
  // (src C-string is likewise intentionally left allocated.)

  logline("[f5] ran external lua: %s (this=0x%08X method=0x%08X)\n", path.c_str(), this_ptr,
          method);
  return true;
}

// Per-frame entry point. Called from the MainRenderLoop_82B9CD68 hook (in
// fps_meter.h). Runs the external file when F5 is latched. This makes F5
// responsive (fires on the very next frame).
inline void poll_mainloop(PPCContext& ctx, uint8_t* base) {
  const bool quick_save = g_f5_pending.exchange(false);
  const bool custom = !quick_save && g_f6_pending.exchange(false);
  // Generic requests run on a frame of their own (never together with F5/F6).
  if (!quick_save && !custom && g_script_has.exchange(false)) {
    std::string path;
    uint32_t seq;
    {
      std::lock_guard<std::mutex> l(script_mtx());
      path.swap(g_script_pending);
      seq = g_script_req_seq.load();
    }
    const PPCContext saved = ctx;
    bool ran = false;
    try {
      ran = run_external(ctx, base, path);
    } catch (...) {
    }
    ctx = saved;
    (ran ? g_script_ran_seq : g_script_fail_seq).store(seq);
    return;
  }
  if (!quick_save && !custom) return;
  const std::string path =
      quick_save ? std::string(REXCVAR_GET(f5_lua_path))
                 : fable2::character_editor::F6Path(std::string(REXCVAR_GET(f6_lua_path)));
  // run_external calls guest functions (ConstructRefCounted, the RunScript
  // method, DestructRefCounted) which clobber the PPC registers. Save the whole
  // context and restore it so the real MainRenderLoop sees the state it had
  // when the hook fired (only F5 detection should be side-effectful).
  logline("[f5] poll_mainloop: r1=0x%08X r2=0x%08X r3=0x%08X (about to run_external)\n",
          (uint32_t)ctx.r1.u64, (uint32_t)ctx.r2.u64, (uint32_t)ctx.r3.u64);
  const PPCContext saved = ctx;
  try {
    run_external(ctx, base, path);
  } catch (...) {
  }
  ctx = saved;
}

}  // namespace fable2::f5lua

// Probe on the shared LuaPlus bound-method dispatcher (named
// "LuaBind_RunScript" by the decompiler, but it is the general thunk every
// Lua->C++ method call goes through). r3 = the bound-object for the specific
// method. We replicate the thunk's wrapper build to read (this, method) and
// decode the string argument (element 1 of the bound-object, whose C-string
// sits at *(bound[12]) + 16). We pin the (this, method) whose argument ends in
// ".lua" — that is the real CScriptManager::RunScript callable.
extern "C" void LuaBind_RunScript_82806168(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::f5lua;
  const uint32_t bound = ctx.r3.u32;  // bound-object for this method call

  if (bound != 0 && bound != 0xFFFFFFFF && ta::rdable2(base, bound, 16)) {
    // Replicate the dispatcher: ProcessAndProcessAndProcess206(-10003, bound)
    // -> wrapper, wrapper[0]=this, wrapper[4]=method.
    const uint64_t save_r3 = ctx.r3.u64;
    ctx.r4.s64 = -10003;
    ctx.r5.u32 = bound;
    REX_CALL_INDIRECT_FUNC(0x822281F8);
    const uint32_t wrapper = ctx.r3.u32;
    ctx.r3.u64 = save_r3;

    uint32_t this_ptr = 0, method = 0;
    if (wrapper != 0 && wrapper != 0xFFFFFFFF && ta::rdable2(base, wrapper, 8)) {
      this_ptr = ta::load_be(base, wrapper + 0);
      method = ta::load_be(base, wrapper + 4);
    }

    // Decode the string argument: element 1 address = bound[12]; the C-string
    // is at *(element1) + 16 (see ProcessAndProcessAndProcess263).
    char pathbuf[96] = {};
    if (ta::rdable2(base, bound + 12, 4)) {
      const uint32_t elem1_addr = ta::load_be(base, bound + 12);
      if (elem1_addr != 0 && elem1_addr != 0xFFFFFFFF && ta::rdable2(base, elem1_addr, 4)) {
        const uint32_t str_data = ta::load_be(base, elem1_addr);
        const uint32_t path_addr = str_data + 16;
        if (path_addr != 0 && ta::rdable2(base, path_addr, sizeof(pathbuf))) {
          std::memcpy(pathbuf, (void*)ta::host_addr(base, path_addr), sizeof(pathbuf) - 1);
        }
      }
    }
    pathbuf[95] = 0;

    if (this_ptr != 0 && method != 0 && pathbuf[0]) {
      const std::string p(pathbuf);
      const bool is_lua = p.size() > 4 && p.compare(p.size() - 4, 4, ".lua") == 0;
      // In-game message acks (src/lua/recomp_notify.lua).
      if (is_lua && p.find("recomp_notify_") != std::string::npos) {
        if (p.find("recomp_notify_shown.lua") != std::string::npos) g_notify_shown.fetch_add(1);
        else if (p.find("recomp_notify_declined.lua") != std::string::npos)
          g_notify_declined.fetch_add(1);
        // Progress marks (recomp_notify_mark.lua) bracket each game API call,
        // so a crash shows the last call reached.
        logline("[notify] ack '%s' (shown=%u declined=%u)\n", pathbuf, g_notify_shown.load(),
                g_notify_declined.load());
      }
      // Character Editor mod (opt-in): the editor asks for a save backup before any change.
      if (is_lua && fable2::character_editor::IsBackupAck(pathbuf)) {
        fable2::character_editor::OnBackupRequest();
      }
      // Capture the RunScript callable from .lua-arg dispatches. IMPORTANT:
      // the shared dispatcher serves MANY bound methods whose string argument
      // is a .lua path (the RunScript file runner AND register-style APIs -
      // e.g. 0x825B27D0 for 'CameraFunctions.lua', 0x825B26A8 for
      // 'AIHistory.lua'). Only the canonical file-runner method (first seen
      // at boot, 0x825ADB40, dispatched for
      // 'miscellaneous/GeneralScriptManager.lua' / 'Quests/QuestManager.lua')
      // actually runs a file - calling the others with a path does nothing.
      // So: the first capture establishes the canonical (method, this); later
      // captures refresh `this` ONLY when the method matches (the manager
      // instance can move over a session); all other methods are ignored.
      const uint32_t prev_this = g_this.load(std::memory_order_relaxed);
      const uint32_t prev_method = g_method.load(std::memory_order_relaxed);
      if (is_lua && (prev_this == 0 || (method == prev_method && this_ptr != prev_this))) {
        g_method.store(method, std::memory_order_relaxed);
        g_this.store(this_ptr, std::memory_order_relaxed);
        const uint32_t head_now =
            ta::rdable2(base, this_ptr, 4) ? ta::load_be(base, this_ptr) : 0;
        g_expected_head.store(head_now, std::memory_order_relaxed);
        logline("[f5] RunScript callable (re)captured this=0x%08X method=0x%08X "
                "head=0x%08X path='%s'\n",
                this_ptr, method, head_now, pathbuf);
      }
    }
  }
  __imp__LuaBind_RunScript_82806168(ctx, base);
}
