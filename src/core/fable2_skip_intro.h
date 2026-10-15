// fable2_skip_intro.h - opt-in skip of the boot logo movies (test-loop speedup).
//
// Boot plays two Bink logo movies from data/art/videos/ before the
// "Press A to start" prompt: microsoft_logo.bik (8.8 s) and lionhead_logo.bik
// (8.3 s); middlewarelogos.bik (6.5 s) is also covered in case it is opened.
//
// Opt-in: FABLE2_SKIP_INTRO_MOVIES=1. Each logo open is redirected to a
// 2-frame copy under <game root>/out/skipintro/ (ms.bik, lh.bik, mw.bik),
// made by out/validation/skip-intro-build/trim_bik.py from the real file
// (trimmed copies of game content: keep them in ignored out/). If the
// redirected open fails, the original path is opened instead, so a missing
// dummy just means the full movie plays. Simply failing the open is NOT safe:
// the game dereferences the null Bink handle (guest AV at 0x0, black screen).
//
// Mechanism: the game's real file opens do not go through a recompiled wrapper
// we can name, so the kernel imports themselves are intercepted. The
// executable's strong __imp__NtCreateFile / __imp__NtOpenFile take precedence
// over librexruntime.dylib's exports for every recompiled caller and the
// import table, and forward to the SDK implementation via dlsym(RTLD_NEXT).
// Both take OBJECT_ATTRIBUTES in r5; +4 points at an ANSI_STRING
// {u16 len, u16 max, u32 buf}. The redirect rewrites that buffer/length in
// place (the replacement path is shorter) for the duration of the call and
// restores it afterwards. No file in data/ is touched.
//
// FABLE2_SKIP_INTRO_TRACE=1 additionally logs every path opened.
//
// Single-TU (included from main.cpp only).

#pragma once

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include <dlfcn.h>

#include <rex/logging/macros.h>

#include "fable2_flight_recorder.h"
#include "fable2_guest_stack.h"
#include "fable2_fast_content_io.h"  // CR-01: overlapped content calls without the 100 ms SDK delay

namespace fable2::skipintro {

using GuestFn = void (*)(PPCContext&, uint8_t*);

inline bool env_on(const char* name) {
  const char* v = std::getenv(name);
  return v && v[0] == '1';
}

inline bool enabled() {
  static const bool on = env_on("FABLE2_SKIP_INTRO_MOVIES");
  return on;
}

inline bool trace() {
  static const bool on = env_on("FABLE2_SKIP_INTRO_TRACE");
  return on;
}

inline bool ends_with_ci(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i])))
      return false;
  }
  return true;
}

// Replacement for a logo movie path, or nullptr.
inline const char* dummy_for(const std::string& p) {
  if (ends_with_ci(p, "\\microsoft_logo.bik")) return "game:\\out\\skipintro\\ms.bik";
  if (ends_with_ci(p, "\\lionhead_logo.bik")) return "game:\\out\\skipintro\\lh.bik";
  if (ends_with_ci(p, "\\middlewarelogos.bik")) return "game:\\out\\skipintro\\mw.bik";
  return nullptr;
}

inline GuestFn next(const char* sym) {
  GuestFn f = reinterpret_cast<GuestFn>(dlsym(RTLD_NEXT, sym));
  if (!f) REXSYS_WARN("[skip-intro] dlsym(RTLD_NEXT, {}) failed", sym);
  return f;
}

// Guest call chain for the trace log lines: the import's LR, then the saved LR of each frame up
// the back chain (callers store LR at prev_sp-8). One shared guarded walker
// (src/diagnostics/fable2_guest_stack.h) serves this, InBootLoader and the flight recorder.
inline std::string guest_stack(PPCContext& ctx, uint8_t* base) {
  char tmp[16];
  std::snprintf(tmp, sizeof tmp, "%08X", static_cast<uint32_t>(ctx.lr));
  std::string out = tmp;
  uint32_t frames[24];
  const int n = fable2::gstack::OwnStack(base, ctx.r1.u32, frames, 24);
  for (int i = 0; i < n; ++i) {
    std::snprintf(tmp, sizeof tmp, " <%08X", frames[i]);
    out += tmp;
  }
  return out;
}

// Flight recorder (io:1): every opened path as an "open" event; a .bik open sets the movie tag
// until its handle is closed (NtClose below).
inline std::atomic<uint32_t>& MovieHandle() {
  static std::atomic<uint32_t> h{0};
  return h;
}
inline void record_open(PPCContext& ctx, uint8_t* base, const std::string& path, uint32_t handle_ptr) {
  if (path.empty() || !fable2::fr::On(fable2::fr::kIo)) return;
  const uint32_t handle = handle_ptr && ctx.r3.s32 >= 0 ? REX_LOAD_U32(handle_ptr) : 0;
  fable2::fr::EmitText(fable2::fr::kTOpen, handle, ctx.r3.u32, 0, path.data(), path.size());
  if (ends_with_ci(path, ".bik") && ctx.r3.s32 >= 0) {
    MovieHandle().store(handle, std::memory_order_relaxed);
    const size_t slash = path.find_last_of('\\');
    fable2::fr::SetTag(fable2::fr::kTagMovie, 1, path.c_str() + (slash == std::string::npos ? 0 : slash + 1));
  }
}

inline void open_hooked(PPCContext& ctx, uint8_t* base, GuestFn real, const char* who) {
  uint32_t str = 0, buf = 0, len = 0;
  std::string path;
  if (const uint32_t oa = ctx.r5.u32) {
    str = REX_LOAD_U32(oa + 4);
    if (str) {
      len = REX_LOAD_U16(str);
      buf = REX_LOAD_U32(str + 4);
      if (len > 0 && len < 512 && buf)
        for (uint32_t i = 0; i < len; ++i) path.push_back(static_cast<char>(REX_LOAD_U8(buf + i)));
    }
  }
  if (trace() && !path.empty())
    REXSYS_INFO("[skip-intro] trace {} '{}' stack {}", who, path, guest_stack(ctx, base));
  const uint32_t handle_ptr = ctx.r3.u32;  // PHANDLE (NtCreateFile / NtOpenFile r3)
  const char* dummy = (enabled() && !path.empty()) ? dummy_for(path) : nullptr;
  if (!dummy || std::strlen(dummy) > len) {
    real(ctx, base);
    record_open(ctx, base, path, handle_ptr);
    return;
  }
  // Redirect: the call may clobber argument registers, so keep a copy.
  const PPCContext saved = ctx;
  const uint32_t dlen = static_cast<uint32_t>(std::strlen(dummy));
  for (uint32_t i = 0; i < dlen; ++i) REX_STORE_U8(buf + i, static_cast<uint8_t>(dummy[i]));
  REX_STORE_U16(str, static_cast<uint16_t>(dlen));
  real(ctx, base);
  for (uint32_t i = 0; i < len; ++i) REX_STORE_U8(buf + i, static_cast<uint8_t>(path[i]));
  REX_STORE_U16(str, static_cast<uint16_t>(len));
  const bool ok = ctx.r3.s32 >= 0;
  REXSYS_INFO("[skip-intro] {} '{}' -> '{}' {}", who, path, dummy,
              ok ? "redirected" : "missing, playing original");
  if (!ok) {
    ctx = saved;
    real(ctx, base);
  }
  record_open(ctx, base, path, handle_ptr);
}

}  // namespace fable2::skipintro

extern "C" void __imp__NtCreateFile(PPCContext& ctx, uint8_t* base) {
  static const auto real = fable2::skipintro::next("__imp__NtCreateFile");
  FR_HEARTBEAT("NtCreateFile");
  fable2::skipintro::open_hooked(ctx, base, real, "NtCreateFile");
}

extern "C" void __imp__NtOpenFile(PPCContext& ctx, uint8_t* base) {
  static const auto real = fable2::skipintro::next("__imp__NtOpenFile");
  FR_HEARTBEAT("NtOpenFile");
  fable2::skipintro::open_hooked(ctx, base, real, "NtOpenFile");
}

// Flight recorder: clears the movie tag when the open .bik handle is closed.
extern "C" void __imp__NtClose(PPCContext& ctx, uint8_t* base) {
  static const auto real = fable2::skipintro::next("__imp__NtClose");
  FR_HEARTBEAT("NtClose");
  const uint32_t handle = ctx.r3.u32;
  auto& movie = fable2::skipintro::MovieHandle();
  if (handle && movie.load(std::memory_order_relaxed) == handle) {
    movie.store(0, std::memory_order_relaxed);
    fable2::fr::SetTag(fable2::fr::kTagMovie, 0);
  }
  real(ctx, base);
}

// Trace-only (FABLE2_SKIP_INTRO_TRACE=1): which save content package is mounted
// and from where. XamContentCreateEx(user r3, root r4, XCONTENT_DATA* r5, flags r6, ...);
// XCONTENT_DATA = {u32 device, u32 type, u16 display_name[128], char file_name[42]}.
extern "C" void __imp__XamContentCreateEx(PPCContext& ctx, uint8_t* base) {
  static const auto real = fable2::skipintro::next("__imp__XamContentCreateEx");
  if (fable2::skipintro::trace()) {
    std::string root, file;
    for (uint32_t i = 0; ctx.r4.u32 && i < 32; ++i) {
      const uint8_t c = REX_LOAD_U8(ctx.r4.u32 + i);
      if (!c) break;
      root.push_back(static_cast<char>(c));
    }
    for (uint32_t i = 0; ctx.r5.u32 && i < 42; ++i) {
      const uint8_t c = REX_LOAD_U8(ctx.r5.u32 + 264 + i);
      if (!c) break;
      file.push_back(static_cast<char>(c));
    }
    REXSYS_INFO("[skip-intro] trace XamContentCreateEx user={} root='{}' file='{}' flags={:#x} stack {}",
                ctx.r3.u32, root, file, ctx.r6.u32, fable2::skipintro::guest_stack(ctx, base));
  }
  if (!fable2::fastcontent::RunCreateEx(ctx, base, real)) real(ctx, base);
}
