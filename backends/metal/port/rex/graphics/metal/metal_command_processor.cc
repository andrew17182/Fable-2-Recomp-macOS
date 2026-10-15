#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_readiness.h"
#include "rex/graphics/metal/metal_trace.h"
#include "rex/graphics/metal/metal_hitch.h"
#include "rex/graphics/metal/metal_converter_census.h"
#include "rex/graphics/metal/metal_air_optimizer.h"
#include "rex/graphics/metal/metal_ns_specialize.h"
#include "rex/graphics/metal/metal_hle_census.h"
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_emu_draw_stats.h"
#include "rex/graphics/metal/metal_ref_tessellator.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_downscale_cs.h"
#include <rex/kernel/xboxkrnl/video.h>
#include <fstream>

// Fable II diagnostic: opt-in, native-resolution morph texture readback.
DEFINE_bool(metal_fable_morph_readback, false,
            "Copy Fable II morph resolves back to guest RAM (native resolution)", "Metal");

DEFINE_int32(metal_probe_capture_interval, 0,
             "Diagnostic only: capture up to six guest frames, every N swaps (0 disables)", "Metal");

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <algorithm>
#include <map>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <objc/message.h>
#include <objc/runtime.h>

#include "third_party/metal-cpp/Foundation/NSURL.hpp"
#include "third_party/metal-cpp/Metal/MTLEvent.hpp"
#include "third_party/metal-cpp/Metal/MTLResidencySet.hpp"

#include "rex/assert.h"
#include "rex/cvar.h"
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/memory/utils.h"
#include "rex/profiling.h"
#include "rex/hash.h"
#include "rex/graphics/util/draw.h"
#include "rex/graphics/flags.h"
#include "rex/graphics/graphics_system.h"
#include "rex/graphics/metal/metal_backend_telemetry.h"
#include "rex/graphics/metal/metal_graphics_system.h"
#include "rex/graphics/metal/metal_encoder_trim.h"
#include "rex/graphics/packet_disassembler.h"
#include "rex/graphics/registers.h"
#include "rex/graphics/pipeline/texture/util.h"
#include "rex/graphics/xenos.h"
#include "rex/system/kernel_state.h"
#include "rex/system/xthread.h"
#include "rex/system/user_module.h"
#include "rex/ui/metal/metal_presenter.h"

// Metal IR Converter Runtime - defines IRDescriptorTableEntry and bind points.
#ifndef IR_RUNTIME_METALCPP
#define IR_RUNTIME_METALCPP
#endif
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(async_shader_compilation);
DECLARE_bool(submit_on_primary_buffer_end);

DEFINE_bool(metal_capture_labels, false,
            "Label GPU captures with guest shader hashes and texture addresses.",
            "Metal");
REXCVAR_DEFINE_UINT32(metal_draw_trace_texture_base, 0, "Metal",
                     "Diagnostic: log up to32 unique draw bindings using this guest texture base.");

DEFINE_bool(metal_fable_terrain_uniform_tessellation, true,
            "Use the guest maximum tessellation factor for Fable II terrain "
            "to reduce cracks at detail transitions.",
            "Metal");

DEFINE_bool(
    metal_async_idle_submission, true,
    "Submit pending Metal work without waiting for GPU completion "
    "when the command processor enters an idle or guest polling wait.",
    "Metal");

DEFINE_bool(
    metal_float_constants_dirty_on_change, true,
    "Only invalidate Metal float constant CBVs when a register write changes a "
    "currently live float constant value. Disable to restore conservative "
    "dirty-on-write behavior.",
    "Metal");

// [gpu-capture] Trace-only (docs/native-shaders.md): FABLE2_GPU_CAPTURE_PATH=<x.gputrace>
// with FABLE2_GPU_CAPTURE_AT_S=<seconds after the first swap> and optional
// FABLE2_GPU_CAPTURE_FRAMES=<n> (default 1) writes an in-process Metal GPU trace of the
// command queue between swaps, for `gpudebug ... profile run`. Needs
// MTL_CAPTURE_ENABLED=1 in the environment. Unset: one getenv at the first swap.
namespace {
void GpuCaptureTick(MTL::CommandQueue* queue) {
  static const char* path = std::getenv("FABLE2_GPU_CAPTURE_PATH");
  if (!path || !path[0] || !queue) {
    return;
  }
  static const double at_s = [] {
    const char* v = std::getenv("FABLE2_GPU_CAPTURE_AT_S");
    return v ? std::atof(v) : 30.0;
  }();
  static const int frames = [] {
    const char* v = std::getenv("FABLE2_GPU_CAPTURE_FRAMES");
    int n = v ? std::atoi(v) : 1;
    return n > 0 ? n : 1;
  }();
  static const auto first = std::chrono::steady_clock::now();
  static int state = 0;  // 0 waiting, 1 capturing, 2 done
  static int captured = 0;
  if (state == 2) {
    return;
  }
  auto* manager = MTL::CaptureManager::sharedCaptureManager();
  if (state == 0) {
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             first)
                   .count();
    if (s < at_s) {
      return;
    }
    state = 2;
    if (!manager->supportsDestination(
            MTL::CaptureDestinationGPUTraceDocument)) {
      REXLOG_WARN("[gpu-capture] GPU trace documents unsupported (set "
                  "MTL_CAPTURE_ENABLED=1)");
      return;
    }
    MTL::CaptureDescriptor* desc = MTL::CaptureDescriptor::alloc()->init();
    desc->setCaptureObject(queue);
    desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
    desc->setOutputURL(NS::URL::fileURLWithPath(
        NS::String::string(path, NS::UTF8StringEncoding)));
    NS::Error* error = nullptr;
    bool ok = manager->startCapture(desc, &error);
    desc->release();
    if (!ok) {
      REXLOG_WARN("[gpu-capture] startCapture failed: {}",
                  error ? error->localizedDescription()->utf8String() : "?");
      return;
    }
    REXLOG_INFO("[gpu-capture] started at {:.1f} s, {} frame(s) -> {}", s,
                frames, path);
    state = 1;
    return;
  }
  if (++captured >= frames) {
    manager->stopCapture();
    state = 2;
    REXLOG_INFO("[gpu-capture] stopped after {} frame(s)", captured);
  }
}
}  // namespace

// [ring-wait] FABLE2_RING_WAIT (docs/weak-mac-perf-20261008.md): the guest 3D Engine
// spins in the D3D read-pointer poll (loop 82242628 -> poll 82B9BF90) until the command
// processor has consumed the ring past a position. The SDK worker writes the read-pointer
// writeback right after ExecutePrimaryBuffer returns, i.e. right after OnPrimaryBufferEnd.
// The executable's poll hook (src/core/fable2_spin_wait.h, override src/core/hotfunc/gpu/RingReadPointerPoll_82B9BF90.cpp) blocks on this progress
// sequence instead of spinning; it finds these two functions with dlsym.
namespace {
std::mutex g_ring_progress_mutex;
std::condition_variable g_ring_progress_cv;
std::atomic<uint64_t> g_ring_progress_seq{0};
std::atomic<uint32_t> g_ring_progress_waiters{0};
void NotifyRingProgress() {
  g_ring_progress_seq.fetch_add(1, std::memory_order_acq_rel);
  if (g_ring_progress_waiters.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(g_ring_progress_mutex);
    g_ring_progress_cv.notify_all();
  }
}
// FABLE2_RING_WAIT_NOSPIN (default on, =0 restores the old order): signal progress from the
// SDK worker right after it stores the read-pointer writeback (fable2_ring_writeback_stored,
// hook added by sdk-metal.patch), not from OnPrimaryBufferEnd just before the store. The
// waiter (src/core/fable2_spin_wait.h, same switch) then sees the new value as soon as it
// wakes and no longer yield-spins up to 200 us for the store after every event.
bool RingNotifyAfterStore() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_RING_WAIT_NOSPIN");
    return !(v && v[0] == '0');
  }();
  return on;
}
}  // namespace
extern "C" void fable2_ring_writeback_stored() {
  if (RingNotifyAfterStore()) {
    NotifyRingProgress();
  }
}
extern "C" __attribute__((visibility("default"))) uint64_t fable2_ring_progress_seq() {
  return g_ring_progress_seq.load(std::memory_order_acquire);
}
// Blocks until the sequence differs from `seen` or `timeout_us` passes. Returns true when
// the command processor reported progress.
extern "C" __attribute__((visibility("default"))) bool fable2_ring_progress_wait(
    uint64_t seen, uint32_t timeout_us) {
  g_ring_progress_waiters.fetch_add(1, std::memory_order_acq_rel);
  bool progressed;
  {
    std::unique_lock<std::mutex> lock(g_ring_progress_mutex);
    progressed = g_ring_progress_cv.wait_for(
        lock, std::chrono::microseconds(timeout_us), [seen] {
          return g_ring_progress_seq.load(std::memory_order_acquire) != seen;
        });
  }
  g_ring_progress_waiters.fetch_sub(1, std::memory_order_acq_rel);
  return progressed;
}

namespace rex {
namespace graphics {
namespace metal {

// [gpu-time-trace] Trace-only (FABLE2_GPU_TIME_TRACE=1, off by default, no behaviour
// change): per command buffer GPUStartTime/GPUEndTime, command-processor idle/GPU waits and
// per-frame CPU of named threads, written to the frame CSV. Events tagged frame F cover the
// interval from guest swap F to swap F+1.
namespace gpu_time_trace {
bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_GPU_TIME_TRACE");
    return v && v[0] == '1';
  }();
  return on && frame_log::Enabled();
}
void AddHandler(MTL::CommandBuffer* cmd, const char* event) {
  if (!cmd || !Enabled()) return;
  const uint64_t frame = frame_log::CurrentFrame();
  cmd->addCompletedHandler([frame, event](MTL::CommandBuffer* done) {
    const double start = done->GPUStartTime(), end = done->GPUEndTime();
    if (start > 0 && end >= start) {
      frame_log::RecordUptime(event, frame, start, uint64_t((end - start) * 1e6));
    }
  });
}
void RecordWait(const char* event, uint64_t start_us) {
  if (Enabled()) frame_log::Record(event, frame_log::CurrentFrame(), frame_log::NowUs() - start_us);
}
uint64_t Now() { return Enabled() ? frame_log::NowUs() : 0; }
// Per-frame CPU of named threads, sampled on the GPU Commands thread at each guest swap.
struct ThreadCpu {
  static constexpr int kCount = 4;
  const char* prefixes[kCount] = {"GPU Commands", "GameThread", "3D Engine", "Audio Worker"};
  const char* events[kCount] = {"tcpu_gpucmd_us", "tcpu_game_us", "tcpu_3d_us", "tcpu_audio_us"};
  mach_port_t ports[kCount] = {};
  uint64_t last[kCount] = {};
  uint64_t last_lookup_us = 0;
  void Lookup() {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
      bool kept = false;
      if (pthread_t pt = pthread_from_mach_thread_np(list[i])) {
        char name[64] = {};
        pthread_getname_np(pt, name, sizeof(name));
        for (int k = 0; k < kCount && !kept; ++k) {
          if (!ports[k] && !std::strncmp(name, prefixes[k], std::strlen(prefixes[k]))) {
            ports[k] = list[i]; last[k] = 0; kept = true;
          }
        }
      }
      if (!kept) mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), vm_address_t(list), count * sizeof(thread_act_t));
  }
  void Sample(uint64_t frame) {
    const uint64_t now = frame_log::NowUs();
    bool missing = false;
    for (int k = 0; k < kCount; ++k) missing |= !ports[k];
    if (missing && now - last_lookup_us > 1000000) { last_lookup_us = now; Lookup(); }
    for (int k = 0; k < kCount; ++k) {
      if (!ports[k]) continue;
      thread_basic_info_data_t info;
      mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
      if (thread_info(ports[k], THREAD_BASIC_INFO, thread_info_t(&info), &n) != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), ports[k]); ports[k] = 0; continue;
      }
      const uint64_t cpu =
          uint64_t(info.user_time.seconds + info.system_time.seconds) * 1000000 +
          info.user_time.microseconds + info.system_time.microseconds;
      if (last[k]) frame_log::Record(events[k], frame, cpu - last[k]);
      last[k] = cpu;
    }
  }
};
// Guest wait sampler (smp_* rows): moved to the flight recorder's all-thread sampler in the
// executable (src/diagnostics/fable2_flight_recorder.cpp; FABLE2_GPU_TIME_TRACE expands to sampler:3,
// i.e. 1 kHz). It reads the guest link register of every guest thread without suspending it and
// keeps the old per-frame counts for 3D Engine (ring/read-pointer spin, 82242628 loop -> poll
// 82B9BF90), GameThread (yield path 82CBD098) and Audio Worker; the "[gpu-time-trace] guest lr top"
// line every 10 s now comes from the recorder too.
struct GuestSamplerRows {
  const char* wait_events[3] = {"smp_3d_wait", "smp_game_wait", "smp_audio_wait"};
  const char* total_events[3] = {"smp_3d_n", "smp_game_n", "smp_audio_n"};
  void Flush(uint64_t frame) {
    const Fable2FrApi* api = trace::Api();
    if (!api) return;
    for (int k = 0; k < 3; ++k) {
      uint32_t n = 0, w = 0;
      api->sampler_counts(k, &n, &w);
      if (n) { frame_log::Record(wait_events[k], frame, w); frame_log::Record(total_events[k], frame, n); }
    }
  }
};
GuestSamplerRows& Sampler() {
  static GuestSamplerRows rows;
  return rows;
}
// Per-frame counters (GPU Commands thread): pending shared-memory write list collapses to
// the whole buffer, and render barriers / fence waits skipped by FABLE2_RENDER_BARRIER_DEDUP.
uint32_t n_collapse = 0, n_barrier_skipped = 0, n_fence_wait_skipped = 0;
void SampleThreadCpu() {
  if (!Enabled()) return;
  static ThreadCpu state;  // GPU Commands thread only
  state.Sample(frame_log::CurrentFrame());
  Sampler().Flush(frame_log::CurrentFrame());
  frame_log::Record("tr_n_shmem_collapse", frame_log::CurrentFrame(), n_collapse);
  frame_log::Record("tr_n_barrier_skipped", frame_log::CurrentFrame(), n_barrier_skipped);
  frame_log::Record("tr_n_fence_wait_skipped", frame_log::CurrentFrame(), n_fence_wait_skipped);
  n_collapse = n_barrier_skipped = n_fence_wait_skipped = 0;
  // [upload-precommit] staged-upload page categories and the render-pass ends they caused.
  UploadTraceCounters& u = UploadTrace();
  const uint64_t f = frame_log::CurrentFrame();
  frame_log::Record("tr_n_upl_pg_current", f, u.pg_current);
  frame_log::Record("tr_n_upl_pg_inflight", f, u.pg_inflight);
  frame_log::Record("tr_n_upl_pg_completed", f, u.pg_completed);
  frame_log::Record("tr_n_upl_pg_standalone", f, u.pg_standalone);
  frame_log::Record("tr_n_upl_brk_current", f, u.brk_current);
  frame_log::Record("tr_n_upl_brk_standalone", f, u.brk_standalone);
  frame_log::Record("tr_n_upl_brk_inflight", f, u.brk_inflight);
  frame_log::Record("tr_n_upl_brk_guard", f, u.brk_guard);
  frame_log::Record("tr_n_upl_precommit_cbs", f, u.precommit_cbs);
  frame_log::Record("tr_n_upl_precommit_pages", f, u.precommit_pages);
  // [texture-precommit] texture-load flushes / loads (UploadTraceCounters).
  frame_log::Record("tr_n_tex_brk", f, u.tex_brk);
  frame_log::Record("tr_n_tex_brk_elig", f, u.tex_brk_elig);
  frame_log::Record("tr_n_tex_ld", f, u.tex_ld);
  frame_log::Record("tr_n_tex_ld_cpu", f, u.tex_ld_cpu);
  frame_log::Record("tr_n_tex_ld_res", f, u.tex_ld_res);
  frame_log::Record("tr_n_tex_ld_scaled", f, u.tex_ld_scaled);
  frame_log::Record("tr_n_tex_ld_first", f, u.tex_ld_first);
  frame_log::Record("tr_n_tex_ld_samp_cur", f, u.tex_ld_samp_cur);
  frame_log::Record("tr_n_tex_ld_res_cur", f, u.tex_ld_res_cur);
  frame_log::Record("tr_n_tex_ld_kb", f, u.tex_ld_kb);
  frame_log::Record("tr_n_tex_ld_small", f, u.tex_ld_small);
  frame_log::Record("tr_n_tex_ld_big", f, u.tex_ld_big);
  frame_log::Record("tr_n_tex_ld_elig", f, u.tex_ld_elig);
  frame_log::Record("tr_n_tex_ld_nopass", f, u.tex_ld_nopass);
  frame_log::Record("tr_n_tex_ld_exec", f, u.tex_ld_exec);
  frame_log::Record("tr_n_tex_brk_blk_samp", f, u.tex_brk_blk_samp);
  frame_log::Record("tr_n_tex_brk_samp_only", f, u.tex_brk_samp_only);
  frame_log::Record("tr_n_trn_rename", f, u.trn_rename);
  frame_log::Record("tr_n_trn_pool_hit", f, u.trn_pool_hit);
  frame_log::Record("tr_n_trn_fb_partial", f, u.trn_fb_partial);
  frame_log::Record("tr_n_trn_fb_keep", f, u.trn_fb_keep);
  frame_log::Record("tr_n_trn_fb_queued", f, u.trn_fb_queued);
  frame_log::Record("tr_n_trn_fb_queued_load", f, u.trn_fb_queued_load);
  frame_log::Record("tr_n_trn_fb_nopass", f, u.trn_fb_nopass);
  frame_log::Record("tr_n_trn_q_only", f, u.trn_q_only);
  frame_log::Record("tr_n_trn_noop_stale", f, u.trn_noop_stale);
  frame_log::Record("tr_n_trn_noop_keep", f, u.trn_noop_keep);
  frame_log::Record("tr_n_nx_same", f, u.nx_same);
  frame_log::Record("tr_n_nx_same_kb", f, u.nx_same_kb);
  frame_log::Record("tr_n_nx_same_renamed", f, u.nx_same_renamed);
  frame_log::Record("tr_n_nx_changed", f, u.nx_changed);
  frame_log::Record("tr_n_trn_fb_other", f, u.trn_fb_other);
  frame_log::Record("tr_n_trn_fb_mem", f, u.trn_fb_mem);
  frame_log::Record("tr_n_trn_fb_kind", f, u.trn_fb_kind);
  frame_log::Record("tr_n_trn_share", f, u.trn_share);
  frame_log::Record("tr_n_trn_settle", f, u.trn_settle);
  frame_log::Record("tr_n_trn_orphan", f, u.trn_orphan);
  frame_log::Record("tr_n_trn_wasted", f, u.trn_wasted);
  frame_log::Record("tr_n_trn_pool_kb", f, u.trn_pool_kb);
  frame_log::Record("tr_n_trn_retired_kb", f, u.trn_retired_kb);
  frame_log::Record("tr_n_tex_brk_blk_res", f, u.tex_brk_blk_res);
  frame_log::Record("tr_n_tex_brk_blk_other", f, u.tex_brk_blk_other);
  frame_log::Record("tr_n_tex_ld_res_gpuw", f, u.tex_ld_res_gpuw);
  frame_log::Record("tr_n_tex_ld_res_copy", f, u.tex_ld_res_copy);
  frame_log::Record("tr_n_tex_ld_res_sa", f, u.tex_ld_res_sa);
  frame_log::Record("tr_n_tex_ld_res_inv", f, u.tex_ld_res_inv);
  frame_log::Record("tr_n_tex_ld_res_pend", f, u.tex_ld_res_pend);
  frame_log::Record("tr_n_tex_ld_scl_elig", f, u.tex_ld_scl_elig);
  frame_log::Record("tr_n_tex_ld_scl_open", f, u.tex_ld_scl_open);
  frame_log::Record("tr_n_tex_ld_scl_nores", f, u.tex_ld_scl_nores);
  frame_log::Record("tr_n_tex_ld_scl_inv", f, u.tex_ld_scl_inv);
  frame_log::Record("tr_n_tex_pre_flush", f, u.tex_pre_flush);
  frame_log::Record("tr_n_tex_pre_ld", f, u.tex_pre_ld);
  frame_log::Record("tr_n_tex_pre_fallback", f, u.tex_pre_fallback);
  frame_log::Record("tr_n_tex_pre_direct", f, u.tex_pre_direct);
  frame_log::Record("tr_n_tex_fb_cur", f, u.tex_fb_cur);
  frame_log::Record("tr_n_tex_fb_inflight", f, u.tex_fb_inflight);
  frame_log::Record("tr_n_tex_fb_sa", f, u.tex_fb_sa);
  frame_log::Record("tr_n_tex_pre_staged", f, u.tex_pre_staged);
  frame_log::Record("tr_n_dc_dst_merged", f, u.dc_dst_merged);
  frame_log::Record("tr_n_rsv_copy", f, u.rsv_copy);
  frame_log::Record("tr_n_rsv_scaled", f, u.rsv_scaled);
  frame_log::Record("tr_n_rsv_grow", f, u.rsv_grow);
  frame_log::Record("tr_n_tex_ld_scl_all", f, u.tex_ld_scl_all);
  frame_log::Record("tr_n_xfer_n", f, u.xfer_n);
  frame_log::Record("tr_n_xfer_rt", f, u.xfer_rt);
  frame_log::Record("tr_n_xfer_same", f, u.xfer_same);
  frame_log::Record("tr_n_xfer_fmt", f, u.xfer_fmt);
  frame_log::Record("tr_n_xfer_cd", f, u.xfer_cd);
  frame_log::Record("tr_n_xfer_msaa", f, u.xfer_msaa);
  frame_log::Record("tr_n_xfer_hostdepth", f, u.xfer_hostdepth);
  frame_log::Record("tr_n_xfer_q", f, u.xfer_q);
  frame_log::Record("tr_n_xfer_fb", f, u.xfer_fb);
  frame_log::Record("tr_n_draw_stencil", f, u.draw_stencil);
  frame_log::Record("tr_n_draw_ds", f, u.draw_ds);
  frame_log::Record("tr_n_eff_draw", f, u.eff_draw);
  frame_log::Record("tr_n_eff_draw_depth", f, u.eff_draw_depth);
  frame_log::Record("tr_n_eff_xfer_native", f, u.eff_xfer_native);
  frame_log::Record("tr_n_eff_xfer_mixed", f, u.eff_xfer_mixed);
  frame_log::Record("tr_n_eff_rsv", f, u.eff_rsv);
  frame_log::Record("tr_n_eff_rsv_mixed", f, u.eff_rsv_mixed);
  frame_log::Record("tr_n_eff_xfer_scaled", f, u.eff_xfer_scaled);
  frame_log::Record("tr_n_eff_rsv_native", f, u.eff_rsv_native);
  frame_log::Record("tr_n_eff_rsv_unified", f, u.eff_rsv_unified);
  frame_log::Record("tr_n_sh_draw", f, u.sh_draw);
  frame_log::Record("tr_n_sh_hostdepth_drop", f, u.sh_hostdepth_drop);
  frame_log::Record("tr_n_ts_draw", f, u.ts_draw);
  frame_log::Record("tr_n_cbend_pbe", f, u.cbend_pbe);
  frame_log::Record("tr_n_cbend_idle", f, u.cbend_idle);
  frame_log::Record("tr_n_cbend_dep", f, u.cbend_dep);
  frame_log::Record("tr_n_cbend_wait", f, u.cbend_wait);
  frame_log::Record("tr_n_xfp_clear", f, u.xfp_clear);
  frame_log::Record("tr_n_xfp_self", f, u.xfp_self);
  frame_log::Record("tr_n_xfp_other", f, u.xfp_other);
  frame_log::Record("tr_n_xfp_kb", f, u.xfp_kb);
  frame_log::Record("tr_n_xfp_clear_kb", f, u.xfp_clear_kb);
  frame_log::Record("tr_n_tile_selfdepth", f, u.tile_selfdepth);
  frame_log::Record("tr_n_dc_deferred", f, u.dc_deferred);
  frame_log::Record("tr_n_dc_merged", f, u.dc_merged);
  frame_log::Record("tr_n_dc_fullrect", f, u.dc_fullrect);
  frame_log::Record("tr_n_dc_fl_resolve", f, u.dc_fl_resolve);
  frame_log::Record("tr_n_dc_fl_xfer", f, u.dc_fl_xfer);
  frame_log::Record("tr_n_dc_fl_reclear", f, u.dc_fl_reclear);
  frame_log::Record("tr_n_dc_fl_preflight", f, u.dc_fl_preflight);
  frame_log::Record("tr_n_dc_fl_guard", f, u.dc_fl_guard);
  frame_log::Record("tr_n_dc_fl_swap", f, u.dc_fl_swap);
  frame_log::Record("tr_n_dc_fl_other", f, u.dc_fl_other);
  frame_log::Record("tr_n_dc_saved_kb", f, u.dc_saved_kb);
  frame_log::Record("tr_n_dc_requeue", f, u.dc_requeue);
  frame_log::Record("tr_n_ac_clear", f, u.ac_clear);
  frame_log::Record("tr_n_ac_surfaces", f, u.ac_surfaces);
  frame_log::Record("tr_n_ac_deferred", f, u.ac_deferred);
  frame_log::Record("tr_n_ac_immediate", f, u.ac_immediate);
  frame_log::Record("tr_n_ac_noop", f, u.ac_noop);
  frame_log::Record("tr_n_ac_fb_state", f, u.ac_fb_state);
  frame_log::Record("tr_n_ac_fb_value", f, u.ac_fb_value);
  frame_log::Record("tr_n_ac_fb_owner", f, u.ac_fb_owner);
  frame_log::Record("tr_n_ac_fb_rect", f, u.ac_fb_rect);
  frame_log::Record("tr_n_ac_cross_format", f, u.ac_cross_format);
  frame_log::Record("tr_n_md_word", f, u.md_word);
  frame_log::Record("tr_n_md_stencil", f, u.md_stencil);
  frame_log::Record("tr_n_md_stencil_xfer", f, u.md_stencil_xfer);
  frame_log::Record("tr_n_md_carried", f, u.md_carried);
  frame_log::Record("tr_n_ssm_clear_la", f, u.ssm_clear_la);
  frame_log::Record("tr_n_fp_variant", f, u.fp_variant);
  frame_log::Record("tr_n_fp_variant_miss", f, u.fp_variant_miss);
  frame_log::Record("tr_n_tres_tile", f, u.tres_tile);
  frame_log::Record("tr_n_tres_offer", f, u.tres_offer);
  frame_log::Record("tr_n_tres_fb_precheck", f, u.tres_fb_precheck);
  frame_log::Record("tr_n_tres_fb_shape", f, u.tres_fb_shape);
  frame_log::Record("tr_n_tres_fb_pass", f, u.tres_fb_pass);
  frame_log::Record("tr_n_tres_fb_area", f, u.tres_fb_area);
  frame_log::Record("tr_n_tres_fb_hazard", f, u.tres_fb_hazard);
  frame_log::Record("tr_n_tres_fb_pso", f, u.tres_fb_pso);
  frame_log::Record("tr_n_tres_fb_tile", f, u.tres_fb_tile);
  frame_log::Record("tr_n_tres_shrunk", f, u.tres_shrunk);
  frame_log::Record("tr_n_tres_grow", f, u.tres_grow);
  frame_log::Record("tr_n_tres_fb_extent", f, u.tres_fb_extent);
  frame_log::Record("tr_n_tres_clear_inpass", f, u.tres_clear_inpass);
  frame_log::Record("tr_n_tres_kept", f, u.tres_kept);
  frame_log::Record("tr_n_tres_kept_break", f, u.tres_kept_break);
  frame_log::Record("tr_n_tres_miplod", f, u.tres_miplod);
  frame_log::Record("tr_n_miplod_bounded", f, u.miplod_bounded);
  frame_log::Record("tr_n_miplod_view", f, u.miplod_view);
  frame_log::Record("tr_n_miplod_unbounded", f, u.miplod_unbounded);
  frame_log::Record("tr_n_xq_inpass", f, u.xq_inpass);
  frame_log::Record("tr_n_xq_reused", f, u.xq_reused);
  frame_log::Record("tr_n_xq_inh_cull", f, u.xq_inh_cull);
  frame_log::Record("tr_n_xq_inh_fill", f, u.xq_inh_fill);
  frame_log::Record("tr_n_xq_inh_bias", f, u.xq_inh_bias);
  frame_log::Record("tr_n_xq_inh_clip", f, u.xq_inh_clip);
  frame_log::Record("tr_n_tex_blit_kb", f, u.tex_blit_kb);
  frame_log::Record("tr_n_tex_blit_scl_kb", f, u.tex_blit_scl_kb);
  frame_log::Record("tr_n_tex_blit_scl", f, u.tex_blit_scl);
  frame_log::Record("tr_n_tex_dl", f, u.tex_dl);
  frame_log::Record("tr_n_tex_dl_kb", f, u.tex_dl_kb);
  frame_log::Record("tr_n_rtt_rsv", f, u.rtt_rsv);
  frame_log::Record("tr_n_rtt_rsv_hit", f, u.rtt_rsv_hit);
  {
    static const char* const kT[6] = {"tr_n_rtt_t0", "tr_n_rtt_t1",
                                      "tr_n_rtt_t2", "tr_n_rtt_t3",
                                      "tr_n_rtt_t4", "tr_n_rtt_t5"};
    static const char* const kL[6] = {"tr_n_rtt_ld0", "tr_n_rtt_ld1",
                                      "tr_n_rtt_ld2", "tr_n_rtt_ld3",
                                      "tr_n_rtt_ld4", "tr_n_rtt_ld5"};
    static const char* const kK[6] = {"tr_n_rtt_ld0_kb", "tr_n_rtt_ld1_kb",
                                      "tr_n_rtt_ld2_kb", "tr_n_rtt_ld3_kb",
                                      "tr_n_rtt_ld4_kb", "tr_n_rtt_ld5_kb"};
    for (int i = 1; i < 6; ++i) frame_log::Record(kT[i], f, u.rtt_t[i]);
    for (int i = 0; i < 6; ++i) {
      frame_log::Record(kL[i], f, u.rtt_ld[i]);
      frame_log::Record(kK[i], f, u.rtt_ld_kb[i]);
    }
  }
  frame_log::Record("tr_n_rtt_direct", f, u.rtt_direct);
  frame_log::Record("tr_n_rtt_direct_kb", f, u.rtt_direct_kb);
  frame_log::Record("tr_n_rtt_keep", f, u.rtt_keep);
  frame_log::Record("tr_n_rtt_keep_kb", f, u.rtt_keep_kb);
  frame_log::Record("tr_n_rtt_waste_kb", f, u.rtt_waste_kb);
  frame_log::Record("tr_n_rtt_mip_direct", f, u.rtt_mip_direct);
  frame_log::Record("tr_n_rtt_mip_keep", f, u.rtt_mip_keep);
  frame_log::Record("tr_n_rtt_inv", f, u.rtt_inv);
  frame_log::Record("tr_n_rtt_scan", f, u.rtt_scan);
  frame_log::Record("tr_n_rtt_trk", f, u.rtt_trk);
  if (u.rts_grow) {
    frame_log::Record("tr_n_rts_grow", f, u.rts_grow);
    frame_log::Record("tr_n_rts_grow_kb", f, u.rts_grow_kb);
  }
  if (u.lz_all_kb) {
    frame_log::Record("tr_n_lz_cand_kb", f, u.lz_cand_kb);
    frame_log::Record("tr_n_lz_dead_kb", f, u.lz_dead_kb);
    frame_log::Record("tr_n_lz_read_kb", f, u.lz_read_kb);
    frame_log::Record("tr_n_lz_morph_kb", f, u.lz_morph_kb);
    frame_log::Record("tr_n_lz_all_kb", f, u.lz_all_kb);
  }
  u = UploadTraceCounters();
}
thread_local uint64_t idle_start_us = 0;
}  // namespace gpu_time_trace

namespace {
constexpr size_t kMaxPendingSharedMemoryWrites = 16;
constexpr size_t kMaxPendingSharedMemoryWriteCapacity = 64;
constexpr size_t kMaxSharedMemoryWaitSegmentsPerPending = 64;
constexpr size_t kMaxCurrentDrawVertexFetchRanges =
    xenos::kVertexFetchConstantCount;

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->drain();
    }
  }

 private:
  NS::AutoreleasePool* pool_ = nullptr;
};

const char* MetalTelemetryCbvSlotName(size_t slot) {
  switch (slot) {
    case 0:
      return "system";
    case 1:
      return "float";
    case 2:
      return "bool_loop";
    case 3:
      return "fetch";
    case 4:
      return "descriptor_indices";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryShaderStageName(size_t stage) {
  switch (stage) {
    case 0:
      return "vertex";
    case 1:
      return "pixel";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootScopeName(size_t stage) {
  switch (stage) {
    case 0:
      return "graphics";
    case 1:
      return "unused";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRenderResourceSetName(size_t set) {
  switch (set) {
    case 0:
      return "fixed";
    case 1:
      return "texture";
    case 2:
      return "root";
    default:
      return "invalid";
  }
}

MTL::RenderStages MetalAllGraphicsRenderStages() {
  return MTL::RenderStages(MTL::RenderStageVertex | MTL::RenderStageFragment |
                           MTL::RenderStageObject | MTL::RenderStageMesh);
}

uint32_t MetalRenderStageBits(MTL::RenderStages stages) {
  return uint32_t(NS::UInteger(stages));
}

uint32_t MetalResourceUsageBits(MTL::ResourceUsage usage) {
  return uint32_t(NS::UInteger(usage));
}

bool MetalObjectRespondsToSelector(const void* object,
                                   const char* selector_name) {
  if (!object || !selector_name) {
    return false;
  }
  SEL selector = sel_registerName(selector_name);
  SEL responds_to_selector = sel_registerName("respondsToSelector:");
  using RespondsToSelectorFn = BOOL (*)(id, SEL, SEL);
  return reinterpret_cast<RespondsToSelectorFn>(objc_msgSend)(
             reinterpret_cast<id>(const_cast<void*>(object)),
             responds_to_selector, selector) != 0;
}

bool MetalResidencySetCvarAllowsEnable(bool supported) {
  const std::string& mode = REXCVAR_GET(metal_residency_sets);
  if (mode == "0" || mode == "false" || mode == "off" || mode == "disabled") {
    return false;
  }
  if (mode == "1" || mode == "true" || mode == "on" || mode == "enabled") {
    return true;
  }
  return supported;
}

const char* MetalTelemetryRootRebuildReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "frame_open";
    case 1:
      return "descriptor_indices_pointer_tuple";
    case 2:
      return "other_cbv_pointer_tuple";
    case 3:
      return "shared_memory_uav_mode";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootSlotsChangedName(size_t bin) {
  switch (bin) {
    case 0:
      return "0";
    case 1:
      return "1";
    case 2:
      return "2";
    case 3:
      return "3";
    case 4:
      return "4";
    case 5:
      return "5_plus";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootRebuildDetailName(size_t detail) {
  switch (detail) {
    case 0:
      return "same_buffer_offset_changed";
    case 1:
      return "different_buffer";
    case 2:
      return "descriptor_indices_only";
    case 3:
      return "other_cbv_only";
    case 4:
      return "mixed_descriptor_and_other";
    case 5:
      return "resource_identity_changed";
    case 6:
      return "resource_identity_same";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryDrawMaterializationSourceName(size_t source) {
  switch (source) {
    case 0:
      return "vertex_fetch";
    case 1:
      return "guest_index";
    case 2:
      return "memexport";
    case 3:
      return "texture_source";
    default:
      return "invalid";
  }
}

const char* MetalPreparedDrawFlushReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "manual";
    case 1:
      return "rt_update";
    case 2:
      return "rt_key_mismatch";
    case 3:
      return "queue_budget";
    case 4:
      return "queue_reject";
    case 5:
      return "prepare_wait";
    case 6:
      return "swap";
    case 7:
      return "copy";
    case 8:
      return "transfer_request";
    case 9:
      return "render_encoder_end";
    case 10:
      return "command_buffer_end";
    case 11:
      return "query";
    default:
      return "invalid";
  }
}

const char* MetalPreparedDrawQueueRejectReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "none";
    case 1:
      return "resident_no_active_queue";
    case 2:
      return "no_shared_memory_ranges";
    case 3:
      return "memexport";
    case 4:
      return "texture_upload";
    case 5:
      return "texture_request_load_data";
    case 6:
      return "pending_draw_pass_transfers";
    case 7:
      return "zpd_active";
    case 8:
      return "rt_key_mismatch";
    case 9:
      return "queue_budget";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootArgSlotName(size_t slot) {
  switch (slot) {
    case 0:
      return "srv0";
    case 1:
      return "srv1";
    case 2:
      return "srv2";
    case 3:
      return "srv3";
    case 4:
      return "srv10";
    case 5:
      return "uav0";
    case 6:
      return "uav1";
    case 7:
      return "uav2";
    case 8:
      return "uav3";
    case 9:
      return "sampler0";
    case 10:
      return "cbv_system";
    case 11:
      return "cbv_float";
    case 12:
      return "cbv_bool_loop";
    case 13:
      return "cbv_fetch";
    case 14:
      return "cbv_descriptor_indices";
    case 15:
      return "cbv_hull_float";
    case 16:
      return "cbv_hull_fetch";
    case 17:
      return "cbv_hull_descriptor_indices";
    case 18:
      return "cbv_domain_float";
    case 19:
      return "cbv_domain_fetch";
    case 20:
      return "cbv_domain_descriptor_indices";
    case 21:
      return "cbv_pixel_float";
    case 22:
      return "cbv_pixel_fetch";
    case 23:
      return "cbv_pixel_descriptor_indices";
    default:
      return "unused";
  }
}

const char* MetalTelemetryRenderEncoderBufferStageName(size_t stage) {
  switch (stage) {
    case 0:
      return "vertex";
    case 1:
      return "fragment";
    case 2:
      return "object";
    case 3:
      return "mesh";
    default:
      return "invalid";
  }
}

struct SharedMemoryRangeSegment {
  uint32_t start;
  uint32_t end;
};

template <typename Visitor>
void ForEachSharedMemoryRangeSegment(
    const MetalCommandProcessor::SharedMemoryRange& range, Visitor&& visitor) {
  if (!range.length) {
    return;
  }
  constexpr uint32_t kSharedMemoryMask = SharedMemory::kBufferSize - 1;
  uint32_t remaining = std::min(range.length, SharedMemory::kBufferSize);
  uint32_t start = range.start & kSharedMemoryMask;
  while (remaining) {
    uint32_t segment_length =
        std::min(remaining, SharedMemory::kBufferSize - start);
    if (segment_length) {
      visitor(SharedMemoryRangeSegment{start, start + segment_length});
    }
    remaining -= segment_length;
    start = 0;
  }
}

bool SharedMemoryRangeOverlapsSegment(
    const MetalCommandProcessor::SharedMemoryRange& range,
    uint32_t segment_start, uint32_t segment_end) {
  if (segment_start >= segment_end) {
    return false;
  }
  bool overlaps = false;
  ForEachSharedMemoryRangeSegment(
      range, [&](SharedMemoryRangeSegment range_segment) {
        overlaps = overlaps || (segment_start < range_segment.end &&
                                segment_end > range_segment.start);
      });
  return overlaps;
}

bool GetTextureSize(MTL::Texture* texture, uint32_t& width_out,
                    uint32_t& height_out) {
  if (!texture) {
    return false;
  }
  width_out = std::max(static_cast<uint32_t>(texture->width()), uint32_t(1));
  height_out = std::max(static_cast<uint32_t>(texture->height()), uint32_t(1));
  return true;
}

bool GetRenderPassDescriptorSize(MTL::RenderPassDescriptor* pass_descriptor,
                                 uint32_t& width_out, uint32_t& height_out) {
  if (!pass_descriptor) {
    return false;
  }

  const uint32_t constrained_width =
      static_cast<uint32_t>(pass_descriptor->renderTargetWidth());
  const uint32_t constrained_height =
      static_cast<uint32_t>(pass_descriptor->renderTargetHeight());
  if (constrained_width && constrained_height) {
    width_out = std::max(constrained_width, uint32_t(1));
    height_out = std::max(constrained_height, uint32_t(1));
    return true;
  }

  if (auto* color_attachments = pass_descriptor->colorAttachments()) {
    for (uint32_t i = 0; i < 8; ++i) {
      auto* attachment = color_attachments->object(i);
      if (attachment &&
          GetTextureSize(attachment->texture(), width_out, height_out)) {
        return true;
      }
    }
  }
  if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
    if (GetTextureSize(depth_attachment->texture(), width_out, height_out)) {
      return true;
    }
  }
  if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
    if (GetTextureSize(stencil_attachment->texture(), width_out, height_out)) {
      return true;
    }
  }
  return false;
}

void GetBoundRenderTargetSize(const MetalRenderTargetCache* render_target_cache,
                              uint32_t fallback_width, uint32_t fallback_height,
                              uint32_t& width_out, uint32_t& height_out) {
  width_out = std::max(fallback_width, uint32_t(1));
  height_out = std::max(fallback_height, uint32_t(1));
  if (!render_target_cache) {
    return;
  }
  MTL::Texture* pass_size_texture = render_target_cache->GetColorTarget(0);
  if (!pass_size_texture) {
    pass_size_texture = render_target_cache->GetDepthTarget();
  }
  if (!pass_size_texture) {
    pass_size_texture = render_target_cache->GetDummyColorTarget();
  }
  if (!pass_size_texture) {
    return;
  }
  GetTextureSize(pass_size_texture, width_out, height_out);
}

void GetActiveRenderTargetSize(
    MTL::RenderPassDescriptor* pass_descriptor,
    const MetalRenderTargetCache* render_target_cache, uint32_t fallback_width,
    uint32_t fallback_height, uint32_t& width_out, uint32_t& height_out) {
  width_out = std::max(fallback_width, uint32_t(1));
  height_out = std::max(fallback_height, uint32_t(1));
  if (GetRenderPassDescriptorSize(pass_descriptor, width_out, height_out)) {
    return;
  }
  GetBoundRenderTargetSize(render_target_cache, fallback_width, fallback_height,
                           width_out, height_out);
}

void ClampScissorToBounds(draw_util::Scissor& scissor, uint32_t width,
                          uint32_t height) {
  width = std::max(width, uint32_t(1));
  height = std::max(height, uint32_t(1));

  scissor.offset[0] = std::min(scissor.offset[0], width);
  scissor.offset[1] = std::min(scissor.offset[1], height);

  uint32_t max_scissor_width = width - scissor.offset[0];
  uint32_t max_scissor_height = height - scissor.offset[1];
  scissor.extent[0] = std::min(scissor.extent[0], max_scissor_width);
  scissor.extent[1] = std::min(scissor.extent[1], max_scissor_height);
}

PipelineAttachmentFormats ResolvePipelineAttachmentFormats(
    const MetalRenderTargetCache* render_target_cache,
    MTL::RenderPassDescriptor* pass_descriptor, bool pixel_shader_writes_depth,
    const char* pipeline_name) {
  PipelineAttachmentFormats result;
  result.sample_count = 1;
  for (uint32_t i = 0; i < 4; ++i) {
    result.color_formats[i] = MTL::PixelFormatInvalid;
  }
  result.depth_format = MTL::PixelFormatInvalid;
  result.stencil_format = MTL::PixelFormatInvalid;

  if (render_target_cache) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache->GetColorTargetForDraw(i)) {
        result.color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          result.sample_count = std::max<uint32_t>(
              result.sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (result.color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache->GetDummyColorTargetForDraw()) {
        result.color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          result.sample_count = std::max<uint32_t>(
              result.sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache->GetDepthTargetForDraw()) {
      result.depth_format = depth_tex->pixelFormat();
      switch (result.depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          result.stencil_format = result.depth_format;
          break;
        default:
          result.stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        result.sample_count =
            std::max<uint32_t>(result.sample_count,
                               static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  if (pass_descriptor) {
    // Rebuild strictly from the active encoder descriptor.
    result.sample_count = 1;
    for (uint32_t i = 0; i < 4; ++i) {
      result.color_formats[i] = MTL::PixelFormatInvalid;
    }
    result.depth_format = MTL::PixelFormatInvalid;
    result.stencil_format = MTL::PixelFormatInvalid;

    auto update_sample_count = [&](MTL::Texture* texture) {
      if (!texture) {
        return;
      }
      NS::UInteger sc = texture->sampleCount();
      if (sc > 0) {
        result.sample_count =
            std::max<uint32_t>(result.sample_count, static_cast<uint32_t>(sc));
      }
    };

    auto* color_attachments = pass_descriptor->colorAttachments();
    for (uint32_t i = 0; i < 4; ++i) {
      auto* attachment =
          color_attachments ? color_attachments->object(i) : nullptr;
      if (!attachment) {
        continue;
      }
      MTL::Texture* texture = attachment->texture();
      if (!texture) {
        continue;
      }
      result.color_formats[i] = texture->pixelFormat();
      update_sample_count(texture);
    }
    if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
      MTL::Texture* texture = depth_attachment->texture();
      if (texture) {
        result.depth_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
    if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
      MTL::Texture* texture = stencil_attachment->texture();
      if (texture) {
        result.stencil_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
    // Propagate combined depth-stencil formats.
    if (result.depth_format != MTL::PixelFormatInvalid &&
        result.stencil_format == MTL::PixelFormatInvalid) {
      switch (result.depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          result.stencil_format = result.depth_format;
          break;
        default:
          break;
      }
    } else if (result.stencil_format != MTL::PixelFormatInvalid &&
               result.depth_format == MTL::PixelFormatInvalid) {
      switch (result.stencil_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          result.depth_format = result.stencil_format;
          break;
        default:
          break;
      }
    }
  }

  // Metal pipeline attachment formats must match the active framebuffer
  // exactly. If no depth attachment is bound, a depth-writing fragment output
  // must not fabricate a depth format in the pipeline descriptor.
  if (pixel_shader_writes_depth &&
      result.depth_format == MTL::PixelFormatInvalid) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN(
          "{}: fragment writes depth without a bound depth attachment; "
          "using no depth attachment in the pipeline descriptor",
          pipeline_name);
    }
  }

  return result;
}

bool ShaderUsesVertexFetch(const Shader& shader) {
  if (!shader.vertex_bindings().empty()) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader.constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
    if (constant_map.vertex_fetch_bitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

bool CollectMetalVertexFetchSharedMemoryRanges(const RegisterFile& regs,
                                               const Shader& vertex_shader,
                                               SharedMemory::Range* ranges,
                                               uint32_t max_ranges,
                                               uint32_t* range_count,
                                               bool strict_logging) {
  *range_count = 0;
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
      switch (vfetch.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          if (strict_logging) {
            REXLOG_WARN(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
                "type. Use --gpu_allow_invalid_fetch_constants to bypass.",
                vfetch_index, vfetch.dword_0, vfetch.dword_1);
          }
          return false;
        default:
          if (strict_logging) {
            REXLOG_WARN("Vertex fetch constant {} ({:08X} {:08X}) is invalid.",
                   vfetch_index, vfetch.dword_0, vfetch.dword_1);
          }
          return false;
      }
      uint32_t buffer_offset = vfetch.address << 2;
      uint32_t buffer_length = vfetch.size << 2;
      if (buffer_offset > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - buffer_offset < buffer_length) {
        if (strict_logging) {
          REXLOG_WARN(
              "Vertex fetch constant {} out of range (offset=0x{:08X} "
              "size={})",
              vfetch_index, buffer_offset, buffer_length);
        }
        return false;
      }
      if (*range_count >= max_ranges) {
        if (strict_logging) {
          REXLOG_WARN("Too many vertex fetch shared-memory ranges");
        }
        return false;
      }
      ranges[(*range_count)++] =
          SharedMemory::Range{buffer_offset, buffer_length};
    }
  }
  return true;
}

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

bool FetchConstantDwordMasksOverlap(
    const DxbcShader::FetchConstantDwordMask& a,
    const DxbcShader::FetchConstantDwordMask& b) {
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] & b[i]) {
      return true;
    }
  }
  return false;
}

bool FetchConstantDwordMaskEmpty(
    const DxbcShader::FetchConstantDwordMask& mask) {
  for (uint32_t word : mask) {
    if (word) {
      return false;
    }
  }
  return true;
}

void MarkAllFetchConstantDwords(DxbcShader::FetchConstantDwordMask& mask) {
  mask.fill(UINT32_MAX);
}

void MarkFetchConstantDword(DxbcShader::FetchConstantDwordMask& mask,
                            uint32_t dword_index) {
  if (dword_index >= DxbcShader::kFetchConstantDwordCount) {
    assert_always();
    return;
  }
  mask[dword_index >> 5] |= uint32_t(1) << (dword_index & 31);
}

void MergeFetchConstantDwordMask(
    DxbcShader::FetchConstantDwordMask& dest,
    const DxbcShader::FetchConstantDwordMask& src) {
  for (size_t i = 0; i < dest.size(); ++i) {
    dest[i] |= src[i];
  }
}

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(
    MetalGraphicsSystem* graphics_system, system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {
  pending_shared_memory_writes_.reserve(kMaxPendingSharedMemoryWriteCapacity);
  pending_shared_memory_writes_scratch_.reserve(
      kMaxPendingSharedMemoryWriteCapacity);
}

MetalCommandProcessor::~MetalCommandProcessor() {
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kShutdown);
  // End any active render encoder before releasing
  // Note: Only call endEncoding if the encoder is still active
  // (not already ended by a committed command buffer)
  if (current_render_encoder_) {
    // The encoder may already be ended if the command buffer was committed
    // In that case, just release it
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
    current_render_encoder_has_zpd_visibility_ = false;
    ResetRenderEncoderBufferBindings();
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  WaitForPendingCompletionHandlers();

  // Release pipeline cache (owns shaders, pipelines, shader translation).
  pipeline_cache_.reset();

  for (auto& pair : depth_stencil_state_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  depth_stencil_state_cache_.clear();

  // Release IR Converter runtime buffers and resources
  if (null_buffer_) {
    null_buffer_->release();
    null_buffer_ = nullptr;
  }
  if (null_texture_) {
    null_texture_->release();
    null_texture_ = nullptr;
  }
  if (null_sampler_) {
    null_sampler_->release();
    null_sampler_ = nullptr;
  }
  current_bindless_stage_root_valid_.fill(false);
  current_bindless_stage_root_serials_.fill(0);
  current_bindless_cbv_gpu_addresses_ = {};
  current_bindless_cbv_buffers_ = {};
  current_bindless_cbv_offsets_ = {};
  current_bindless_active_cbv_masks_.fill(0);
  current_bindless_fixed_resource_set_ = {};
  current_bindless_texture_resource_set_ = {};
  current_bindless_root_resource_set_ = {};
  current_bindless_fixed_resource_source_serial_ = 0;
  current_bindless_texture_resource_source_serial_ = 0;
  current_bindless_root_resource_source_serial_ = 0;
  render_encoder_bindless_fixed_resources_serial_ = 0;
  render_encoder_bindless_texture_resources_serial_ = 0;
  render_encoder_bindless_root_resources_serial_ = 0;
  render_encoder_bindless_stage_root_bind_serials_.fill(0);
  render_encoder_bindless_table_bind_mesh_path_ = false;
  render_encoder_bindless_table_bind_tessellation_ = false;
  current_bindless_stage_root_arguments_ = {};
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Restore the guest EDRAM snapshot captured in the trace into the Metal
  // render-target cache so that subsequent host render targets created from
  // EDRAM (via LoadTiledData) see the same initial contents as other
  // backends like D3D12.
  if (!snapshot) {
    REXLOG_WARN(
        "MetalCommandProcessor::RestoreEdramSnapshot called with null "
        "snapshot");
    return;
  }
  if (!render_target_cache_) {
    REXLOG_WARN(
        "MetalCommandProcessor::RestoreEdramSnapshot called before render "
        "target "
        "cache initialization");
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // TODO(wmarti): Add cache_clear_requested_ flag like D3D12 for deferred
  // clearing of pipeline caches, texture caches, etc.
}

void MetalCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void MetalCommandProcessor::ClearReadbackBuffers() {
  // TODO(wmarti): Implement readback buffer clearing when memexport readback
  // is added. See D3D12's readback_buffers_ and memexport_readback_buffers_.
}

ui::metal::MetalProvider& MetalCommandProcessor::GetMetalProvider() const {
  return *static_cast<ui::metal::MetalProvider*>(graphics_system_->provider());
}

uint64_t MetalCommandProcessor::GetCurrentSubmission() const {
  return submission_current_ ? submission_current_ : 1;
}

uint64_t MetalCommandProcessor::GetCompletedSubmission() const {
  return completed_command_buffers_.load(std::memory_order_relaxed);
}

// [upload-precommit] causes of render-pass ends made by staged uploads.
namespace {
enum UploadBreakCause : int {
  kUploadBreakNone = 0,
  kUploadBreakCurrent,     // a page the open command buffer read (WAR)
  kUploadBreakStandalone,  // a page a standalone command buffer still uses
  kUploadBreakInflight,    // only committed command buffers (avoidable)
  kUploadBreakGuard,       // as inflight, but the pre-commit route was not allowed
  kUploadBreakOther,       // direct writes off / no shared contents
  kUploadBreakCauseCount,
};
const char* UploadBreakDetail(int cause, bool prepare_path) {
  static const char* const kPrepare[kUploadBreakCauseCount] = {
      "upload_pre:unknown",    "upload_pre:war_current",
      "upload_pre:standalone", "upload_pre:inflight",
      "upload_pre:guard",      "upload_pre:other"};
  static const char* const kTransfer[kUploadBreakCauseCount] = {
      "upload_xfer:unknown",    "upload_xfer:war_current",
      "upload_xfer:standalone", "upload_xfer:inflight",
      "upload_xfer:guard",      "upload_xfer:other"};
  if (cause < 0 || cause >= kUploadBreakCauseCount) cause = 0;
  return prepare_path ? kPrepare[cause] : kTransfer[cause];
}
int UploadBreakCauseOf(MetalSharedMemory::PageAccess access, bool guard) {
  switch (access) {
    case MetalSharedMemory::PageAccess::kCurrent:
      return kUploadBreakCurrent;
    case MetalSharedMemory::PageAccess::kStandalone:
      return kUploadBreakStandalone;
    case MetalSharedMemory::PageAccess::kInflight:
      return guard ? kUploadBreakGuard : kUploadBreakInflight;
    default:
      return kUploadBreakOther;
  }
}
}  // namespace

// Diagnostics only (unsafe): FABLE2_UPLOAD_PRECOMMIT_DIAG=nowar skips the
// pre-commit command buffer's wait for the last reader (WAR), to measure what
// that wait costs. Never on by default.
static bool PrecommitDiagSkipWar() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_UPLOAD_PRECOMMIT_DIAG");
    const bool skip = v && std::strcmp(v, "nowar") == 0;
    if (skip) REXLOG_WARN("[upload-precommit] DIAG nowar: WAR wait skipped (unsafe)");
    return skip;
  }();
  return on;
}

bool MetalCommandProcessor::SharedMemoryPrecommitAllowed() const {
  // Only worth it (and only used) while a render pass is open: otherwise the
  // in-command-buffer blit ends nothing. Every condition below keeps the
  // pre-commit blit from moving ahead of GPU work that is encoded but whose
  // reads are not yet recorded in the page tracking:
  // - prepared draws not yet encoded (the in-CB path flushes them first);
  // - texture uploads deferred or in an uncommitted upload-batch command
  //   buffer (the in-CB path flushes them at the encoder boundary).
  return MetalSharedMemory::UploadPrecommitEnabled() &&
         (!texture_precommit_active_ || shmem_precommit_in_tex_precommit_) &&
         main_cb_event_ &&
         precommit_event_ && precommit_fence_ && current_command_buffer_ &&
         current_render_encoder_ && !shared_memory_upload_blit_encoder_ &&
         prepared_draw_queue_.empty() &&
         !(texture_cache_ && texture_cache_->HasPendingUploadWork());
}

bool MetalCommandProcessor::EnsurePrecommitCommandBuffer() {
  // One pre-commit command buffer per main command buffer: it collects every
  // pre-commit copy (staged shared-memory uploads, [texture-precommit]
  // texture loads) made while the current command buffer is encoded and is
  // committed just before it (CommitSharedMemoryPrecommit). Nothing in the
  // current command buffer runs before that commit, so later copies still
  // land before the draws that need them.
  if (precommit_cb_) {
    return true;
  }
  precommit_cb_ =
      CreateStandaloneTransferCommandBuffer("XeniaSharedMemoryPrecommit");
  if (!precommit_cb_) {
    return false;
  }
  // WAR: pages and textures routed here were last read by committed main
  // command buffers that may still be running. Main command buffers signal
  // their index on main_cb_event_ when they finish; wait for the newest
  // committed one, which covers everything this command buffer will take (no
  // main command buffer is committed while the current one is open).
  const uint64_t war_wait = GetLastCommittedSubmission();
  if (war_wait && !PrecommitDiagSkipWar()) {
    precommit_cb_->encodeWait(main_cb_event_, war_wait);
  }
  // The value this command buffer signals when its work is done.
  precommit_open_value_ = ++precommit_event_value_;
  precommit_fence_updates_ = 0;
  ++UploadTrace().precommit_cbs;
  return true;
}

void MetalCommandProcessor::PrecommitEncoderBegin(
    MTL::BlitCommandEncoder* encoder) {
  // Chain the pre-commit encoders: each one starts after the previous one
  // updated the fence, so the last update (the one the render encoder waits
  // for) implies all of them.
  if (encoder && precommit_fence_updates_) {
    encoder->waitForFence(precommit_fence_);
  }
}
void MetalCommandProcessor::PrecommitEncoderBegin(
    MTL::ComputeCommandEncoder* encoder) {
  if (encoder && precommit_fence_updates_) {
    encoder->waitForFence(precommit_fence_);
  }
}
void MetalCommandProcessor::PrecommitEncoderEnd(
    MTL::BlitCommandEncoder* encoder) {
  if (encoder) {
    encoder->updateFence(precommit_fence_);
    ++precommit_fence_updates_;
  }
}
void MetalCommandProcessor::PrecommitEncoderEnd(
    MTL::ComputeCommandEncoder* encoder) {
  if (encoder) {
    encoder->updateFence(precommit_fence_);
    ++precommit_fence_updates_;
  }
}

void MetalCommandProcessor::EndPrecommitBlit() {
  if (!precommit_blit_) {
    return;
  }
  PrecommitEncoderEnd(precommit_blit_);
  precommit_blit_->endEncoding();
  precommit_blit_->release();
  precommit_blit_ = nullptr;
}

MTL::BlitCommandEncoder* MetalCommandProcessor::BeginSharedMemoryPrecommit(
    uint64_t wait_submission) {
  (void)wait_submission;  // covered by the newest committed index
  if (precommit_blit_) {
    return precommit_blit_;
  }
  if (!EnsurePrecommitCommandBuffer()) {
    return nullptr;
  }
  // [tex-upload-merge] close parked texture encoders first (one open encoder per command
  // buffer; the copy then also stays ordered after the texture loads before it).
  if (texture_cache_ && !texture_cache_->FlushParkedPrecommitUploads()) {
    return nullptr;
  }
  precommit_blit_ = pass_trace::Blit(precommit_cb_, "shmem_precommit");
  if (!precommit_blit_) {
    return nullptr;
  }
  precommit_blit_->retain();
  PrecommitEncoderBegin(precommit_blit_);
  return precommit_blit_;
}

void MetalCommandProcessor::WaitPrecommitInRenderEncoder() {
  // RAW for the active render pass: wait (once per render encoder) on the
  // fence the pre-commit encoders update. The pre-commit command buffer is
  // committed before the current one, as fences require.
  if (current_render_encoder_ &&
      precommit_fence_waited_encoder_gen_ != render_encoder_gen_) {
    current_render_encoder_->waitForFence(precommit_fence_,
                                          MetalAllGraphicsRenderStages());
    pass_trace::NoteFenceWait(current_render_pass_trace_);
    precommit_fence_waited_encoder_gen_ = render_encoder_gen_;
  }
}

void MetalCommandProcessor::SetShmemPrecommitInTexturePrecommit(bool on) {
  // [pass-merge] window in which a texture flush on the pre-commit route may
  // send its shared-memory upload to the pre-commit command buffer too. On
  // close, end that blit: the flush's loads open their own encoders in the
  // same command buffer next (one open encoder per command buffer) and wait
  // on the fence the blit updates.
  shmem_precommit_in_tex_precommit_ = on && texture_precommit_active_;
  if (!on) {
    EndPrecommitBlit();
  }
}

void MetalCommandProcessor::EndSharedMemoryPrecommit(uint32_t pages) {
  if (!precommit_blit_ || !pages) {
    return;
  }
  WaitPrecommitInRenderEncoder();
  UploadTrace().precommit_pages += pages;
}

void MetalCommandProcessor::CommitSharedMemoryPrecommit() {
  if (!precommit_cb_) {
    return;
  }
  if (texture_cache_) {
    texture_cache_->FlushParkedPrecommitUploads();  // [tex-upload-merge]
  }
  EndPrecommitBlit();
  precommit_cb_->encodeSignalEvent(precommit_event_, precommit_open_value_);
  const uint64_t value = precommit_open_value_;
  std::atomic<uint64_t>* completed = &precommit_completed_value_;
  precommit_cb_->addCompletedHandler([completed, value](MTL::CommandBuffer*) {
    uint64_t prev = completed->load(std::memory_order_relaxed);
    while (prev < value && !completed->compare_exchange_weak(
                               prev, value, std::memory_order_release)) {
    }
  });
  CommitStandaloneAsync(precommit_cb_);
  precommit_cb_ = nullptr;
  precommit_committed_value_ = precommit_open_value_;
}

void MetalCommandProcessor::EncodePrecommitWait(MTL::CommandBuffer* cmd) {
  // Current main command buffer only: later encoders wait for the pre-commit
  // copies (including the open pre-commit command buffer, committed first).
  if (!cmd || !precommit_event_ ||
      precommit_waited_value_ >= precommit_event_value_) {
    return;
  }
  cmd->encodeWait(precommit_event_, precommit_event_value_);
  precommit_waited_value_ = precommit_event_value_;
}

void MetalCommandProcessor::EncodePrecommitWaitCommitted(
    MTL::CommandBuffer* cmd) {
  // New command buffers: wait only for committed pre-commit command buffers
  // that may still be running (never for an open one, which is committed
  // later on the same queue).
  if (!cmd || !precommit_event_ || !precommit_committed_value_ ||
      precommit_completed_value_.load(std::memory_order_acquire) >=
          precommit_committed_value_) {
    return;
  }
  cmd->encodeWait(precommit_event_, precommit_committed_value_);
}

void MetalCommandProcessor::EncodeMainCommandBufferEnd() {
  if (!current_command_buffer_) {
    return;
  }
  CommitSharedMemoryPrecommit();
  // Also keeps the current command buffer's completion (and the release of
  // the pre-commit staging buffers tagged with its submission) after the
  // pre-commit copies.
  EncodePrecommitWait(current_command_buffer_);
  if (main_cb_event_) {
    current_command_buffer_->encodeSignalEvent(main_cb_event_,
                                               submission_current_);
  }
}

void MetalCommandProcessor::NoteSharedMemoryUploadBreakCause(
    MetalSharedMemory::PageAccess cause, bool guard_blocked) {
  pending_upload_break_cause_ = UploadBreakCauseOf(cause, guard_blocked);
}

void MetalCommandProcessor::CountUploadBreak(int cause, bool prepare_path) {
  render_end_detail_ = UploadBreakDetail(cause, prepare_path);
  UploadTraceCounters& u = UploadTrace();
  switch (cause) {
    case kUploadBreakCurrent:
      ++u.brk_current;
      break;
    case kUploadBreakStandalone:
      ++u.brk_standalone;
      break;
    case kUploadBreakInflight:
      ++u.brk_inflight;
      break;
    case kUploadBreakGuard:
      ++u.brk_guard;
      break;
    default:
      break;
  }
}

void MetalCommandProcessor::ForceIssueSwap() {
  // Force a swap to push any pending render target to presenter
  // This is used by trace dumps to capture output when there's no explicit swap
  if (saw_swap_) {
    return;
  }
  frame_log::SetSynthetic(true);
  IssueSwap(0, 1280, 720);
  frame_log::SetSynthetic(false);
}

void MetalCommandProcessor::SetSwapDestSwap(uint32_t dest_base, bool swap) {
  if (!dest_base) {
    return;
  }
  if (swap_dest_swaps_by_base_.size() > 256) {
    swap_dest_swaps_by_base_.clear();
  }
  swap_dest_swaps_by_base_[dest_base] = swap;
}

bool MetalCommandProcessor::ConsumeSwapDestSwap(uint32_t dest_base,
                                                bool* swap_out) {
  if (!swap_out || !dest_base) {
    return false;
  }
  auto it = swap_dest_swaps_by_base_.find(dest_base);
  if (it == swap_dest_swaps_by_base_.end()) {
    return false;
  }
  *swap_out = it->second;
  swap_dest_swaps_by_base_.erase(it);
  return true;
}

void MetalCommandProcessor::InitializeResidencySet() {
  residency_set_supported_ = false;
  residency_set_enabled_ = false;
  residency_set_attached_ = false;
  residency_set_resources_.clear();
  residency_set_heaps_.clear();
  if (!device_ || !command_queue_) {
    return;
  }

  residency_set_supported_ =
      MetalObjectRespondsToSelector(device_,
                                    "newResidencySetWithDescriptor:error:") &&
      MetalObjectRespondsToSelector(command_queue_, "addResidencySet:") &&
      MetalObjectRespondsToSelector(command_queue_, "removeResidencySet:");
  if (!MetalResidencySetCvarAllowsEnable(residency_set_supported_)) {
    return;
  }
  if (!residency_set_supported_) {
    REXLOG_WARN(
        "Metal residency sets requested, but this Metal runtime does not "
        "expose MTLResidencySet");
    return;
  }

  MTL::ResidencySetDescriptor* descriptor =
      MTL::ResidencySetDescriptor::alloc()->init();
  descriptor->setLabel(
      NS::String::string("XeniaResidencySet", NS::UTF8StringEncoding));
  descriptor->setInitialCapacity(512);
  NS::Error* error = nullptr;
  residency_set_ = device_->newResidencySet(descriptor, &error);
  descriptor->release();
  if (!residency_set_) {
    REXLOG_WARN("Metal residency set creation failed: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown");
    return;
  }

  command_queue_->addResidencySet(residency_set_);
  residency_set_enabled_ = true;
  residency_set_attached_ = true;
}

void MetalCommandProcessor::ShutdownResidencySet() {
  if (residency_set_attached_ && command_queue_ && residency_set_) {
    command_queue_->removeResidencySet(residency_set_);
  }
  residency_set_attached_ = false;
  residency_set_enabled_ = false;
  residency_set_resources_.clear();
  residency_set_heaps_.clear();
  if (residency_set_) {
    residency_set_->removeAllAllocations();
    residency_set_->commit();
    residency_set_->release();
    residency_set_ = nullptr;
  }
}

bool MetalCommandProcessor::AddResidencySetResource(MTL::Resource* resource) {
  if (!residency_set_enabled_ || !residency_set_ || !resource) {
    return false;
  }
  auto [it, inserted] = residency_set_resources_.insert(resource);
  if (!inserted) {
    ++backend_telemetry_.residency_set_allocation_duplicates;
    return true;
  }
  residency_set_->addAllocation(resource);
  residency_set_->commit();
  ++backend_telemetry_.residency_set_allocations_added;
  ++backend_telemetry_.residency_set_commits;
  return true;
}

bool MetalCommandProcessor::IsResidencySetResourceCovered(
    MTL::Resource* resource) const {
  if (!residency_set_enabled_ || !resource) {
    return false;
  }
  // TODO (xenios-jp): Revisit heap-backed texture / render-target
  // coverage only after the backend owns the full hazard model for those
  // allocations. MTLResidencySet proves that an allocation is resident, but it
  // doesn't replace the encoder usage declarations the current texture and
  // render-target paths rely on for hazard visibility. A safe removal of
  // per-encoder useResource / useHeap for heap-backed allocations needs:
  //  * read/write tracking for texture-cache and render-target-cache resources,
  //    including color, depth, stencil, MSAA, resolve, and transfer views;
  //  * parent-resource / texture-view / heap alias handling so subresource use
  //    is attributed to the actual allocation that may conflict;
  //  * explicit ordering between render, compute, and blit encoders with
  //    fences, events, or conservative encoder boundaries before conflicting
  //    uses;
  //  * validation coverage for unpooled textures, memoryless attachments, and
  //    direct device->newTexture fallback paths.
  //
  // Until that exists, only resources explicitly added to
  // residency_set_resources_ are considered safe to suppress here. Heap-backed
  // textures and render targets must still go through the encoder usage path.
  return residency_set_resources_.count(resource) != 0;
}

bool MetalCommandProcessor::AddResidencySetHeap(MTL::Heap* heap) {
  if (!residency_set_enabled_ || !residency_set_ || !heap) {
    return false;
  }
  auto [it, inserted] = residency_set_heaps_.insert(heap);
  if (!inserted) {
    ++backend_telemetry_.residency_set_allocation_duplicates;
    return true;
  }
  residency_set_->addAllocation(heap);
  residency_set_->commit();
  ++backend_telemetry_.residency_set_allocations_added;
  ++backend_telemetry_.residency_set_commits;
  return true;
}

bool MetalCommandProcessor::IsResidencySetHeapCovered(MTL::Heap* heap) const {
  return residency_set_enabled_ && heap &&
         residency_set_heaps_.find(heap) != residency_set_heaps_.end();
}

void MetalCommandProcessor::RegisterInitialResidencySetResources() {
  AddResidencySetResource(shared_memory_ ? shared_memory_->GetBuffer()
                                         : nullptr);
  AddResidencySetResource(
      render_target_cache_ ? render_target_cache_->GetEdramBuffer() : nullptr);
  AddResidencySetResource(tessellator_tables_buffer_);
  AddResidencySetResource(null_buffer_);
  AddResidencySetResource(null_texture_);
  AddResidencySetResource(view_bindless_heap_);
  AddResidencySetResource(sampler_bindless_heap_);
  AddResidencySetResource(system_view_tables_);
}

bool MetalCommandProcessor::SetupContext() {
  // FABLE2_GPU_CMD_QOS (default on, =0 off; docs/weak-mac-perf-20261008.md): SetupContext
  // runs on the GPU Commands worker. It is the CPU critical path (translation + Metal
  // encoding, ~5 ms per frame), so ask for user-interactive QoS to keep it on a P-core.
  {
    const char* v = std::getenv("FABLE2_GPU_CMD_QOS");  // getenv-ok: once, at SetupContext
    if (!(v && v[0] == '0')) {
      qos_class_t before = QOS_CLASS_UNSPECIFIED;
      int rel = 0;
      pthread_get_qos_class_np(pthread_self(), &before, &rel);
      const int r = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
      REXLOG_INFO("[gpu-cmd-qos] GPU Commands thread QoS 0x{:x} -> user-interactive: {}",
                  uint32_t(before), r == 0 ? "ok" : fmt::format("failed ({})", r));
    } else {
      REXLOG_INFO("[gpu-cmd-qos] off (FABLE2_GPU_CMD_QOS=0)");
    }
  }
  hle_census_hooks_ = hle_census::Enabled() ||
                      hle_census::Collapse() != hle_census::CollapseMode::kOff;
  if (hle_census::Collapse() != hle_census::CollapseMode::kOff) {
    REXLOG_WARN("[tiling-collapse] FABLE2_TILING_COLLAPSE=skip2: UNSAFE diagnostic, second "
                "EDRAM tile skipped (bottom rows stale)");
  }
  saw_swap_ = false;
  last_swap_ptr_ = 0;
  last_swap_width_ = 0;
  last_swap_height_ = 0;
  swap_dest_swaps_by_base_.clear();
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  if (!CommandProcessor::SetupContext()) {
    REXLOG_ERROR("Failed to initialize base command processor context");
    return false;
  }

  const ui::metal::MetalProvider& provider = GetMetalProvider();
  device_ = provider.GetDevice();
  command_queue_ = provider.GetCommandQueue();
  pass_trace::Initialize(device_);  // [pass-trace] no-op unless FABLE2_GPU_TIME_TRACE=1

  if (!device_ || !command_queue_) {
    REXLOG_ERROR("MetalCommandProcessor: No Metal device or command queue available");
    return false;
  }

  InitializeResidencySet();

  wait_shared_event_ = device_->newSharedEvent();
  if (wait_shared_event_) {
    wait_shared_event_->setLabel(
        NS::String::string("XeniaWaitEvent", NS::UTF8StringEncoding));
    wait_shared_event_value_ = 0;
  } else {
    REXLOG_WARN(
        "MetalCommandProcessor: SharedEvent unavailable; falling back to "
        "waitUntilCompleted");
  }

  if (MetalSharedMemory::UploadPrecommitEnabled()) {  // logs its state once
    main_cb_event_ = device_->newEvent();
    precommit_event_ = device_->newEvent();
    precommit_fence_ = device_->newFence();
    if (!main_cb_event_ || !precommit_event_ || !precommit_fence_) {
      REXLOG_WARN("[upload-precommit] events/fence unavailable; route disabled");
    } else {
      main_cb_event_->setLabel(
          NS::String::string("XeniaMainCbEvent", NS::UTF8StringEncoding));
      precommit_event_->setLabel(
          NS::String::string("XeniaPrecommitEvent", NS::UTF8StringEncoding));
    }
  }

  shared_memory_fence_ = device_->newFence();
  if (!shared_memory_fence_) {
    REXLOG_ERROR(
        "MetalCommandProcessor: MTLFence unavailable; cannot safely order "
        "shared-memory GPU write hazards");
    return false;
  }
  shared_memory_fence_->setLabel(
      NS::String::string("XeniaSharedMemoryFence", NS::UTF8StringEncoding));

  bool supports_apple7 = device_->supportsFamily(MTL::GPUFamilyApple7);
  bool supports_mac2 = device_->supportsFamily(MTL::GPUFamilyMac2);
  mesh_shader_supported_ = supports_apple7 || supports_mac2;

  // Initialize shared memory
  shared_memory_ = std::make_unique<MetalSharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    REXLOG_ERROR("Failed to initialize shared memory");
    return false;
  }

  // Initialize primitive processor (index/primitive conversion like D3D12).
  primitive_processor_ = std::make_unique<MetalPrimitiveProcessor>(
      *this, *register_file_, *memory_, trace_writer_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    REXLOG_ERROR("Failed to initialize Metal primitive processor");
    return false;
  }

  // Create persistent bindless descriptor heaps BEFORE the texture/sampler
  // caches, because their Initialize() allocates persistent heap slots for
  // null textures and samplers.
  view_bindless_heap_ = device_->newBuffer(
      kViewBindlessHeapSize * sizeof(IRDescriptorTableEntry),
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!view_bindless_heap_) {
    REXLOG_ERROR("Failed to create view bindless heap");
    return false;
  }
  view_bindless_heap_->setLabel(
      NS::String::string("XeniaViewBindlessHeap", NS::UTF8StringEncoding));
  memset(view_bindless_heap_->contents(), 0, view_bindless_heap_->length());

  sampler_bindless_heap_ = device_->newBuffer(
      kSamplerBindlessHeapSize * sizeof(IRDescriptorTableEntry),
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!sampler_bindless_heap_) {
    REXLOG_ERROR("Failed to create sampler bindless heap");
    return false;
  }
  sampler_bindless_heap_->setLabel(
      NS::String::string("XeniaSamplerBindlessHeap", NS::UTF8StringEncoding));
  memset(sampler_bindless_heap_->contents(), 0,
         sampler_bindless_heap_->length());

  // The bindless heaps are dedicated to dynamically indexed textures and
  // samplers only. System descriptors such as shared memory / EDRAM use small
  // explicit-layout top-level tables populated below once all resources exist.
  view_bindless_heap_next_ = 0;
  view_bindless_heap_exhausted_logged_ = false;
  sampler_bindless_heap_next_ = 0;
  sampler_bindless_heap_exhausted_logged_ = false;

  // Experimental draw resolution scale (investigation, docs/resolution-scale.md).
  // Native (1x1) unless FABLE2_RES_SCALE=N (N = 1..3, both axes) is set, or
  // FABLE2_RES_SCALE=cvar, which honours the SDK cvars draw_resolution_scale_x/y
  // / resolution_scale. Clamped to 3 per axis.
  uint32_t res_scale_x = 1, res_scale_y = 1;
  if (const char* v = std::getenv("FABLE2_RES_SCALE"); v && *v) {  // getenv-ok: once, at SetupContext
    if (std::strcmp(v, "cvar") == 0) {
      TextureCache::GetConfigDrawResolutionScale(res_scale_x, res_scale_y);
    } else {
      int n = std::atoi(v);
      res_scale_x = res_scale_y = uint32_t(std::max(1, n));
    }
    res_scale_x = std::min<uint32_t>(res_scale_x, 3);
    res_scale_y = std::min<uint32_t>(res_scale_y, 3);
    REXLOG_INFO("[res-scale] FABLE2_RES_SCALE={} -> draw resolution scale {}x{}",
                v, res_scale_x, res_scale_y);
  }

  const auto setup_caches_start = std::chrono::steady_clock::now();
  texture_cache_ = std::make_unique<MetalTextureCache>(
      this, *register_file_, *shared_memory_, res_scale_x, res_scale_y);
  if (!texture_cache_->Initialize()) {
    REXLOG_ERROR("Failed to initialize Metal texture cache");
    return false;
  }
  // [rtt-prewarm] fused resolve pipelines, built beside the rest of startup.
  texture_cache_->StartRttPrewarm();
  const auto setup_rt_start = std::chrono::steady_clock::now();

  // Initialize render target cache
  render_target_cache_ = std::make_unique<MetalRenderTargetCache>(
      *register_file_, *memory_, &trace_writer_, res_scale_x, res_scale_y,
      *this);
  if (!render_target_cache_->Initialize()) {
    REXLOG_ERROR("Failed to initialize Metal render target cache");
    return false;
  }
  REXLOG_INFO(
      "[startup-par] setup: texture cache {:.0f} ms, render-target cache {:.0f} "
      "ms",
      std::chrono::duration<double, std::milli>(setup_rt_start -
                                                setup_caches_start)
          .count(),
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - setup_rt_start)
          .count());

  // Create and initialize pipeline cache (shader translation + pipeline
  // management).
  pipeline_cache_ =
      std::make_unique<MetalPipelineCache>(device_, *register_file_);
  {
    bool edram_rov_used = false;
    bool gamma_render_target_as_unorm8 =
        !(edram_rov_used ||
          render_target_cache_->gamma_render_target_as_unorm16());
    if (!pipeline_cache_->InitializeShaderTranslation(
            gamma_render_target_as_unorm8,
            render_target_cache_->msaa_2x_supported(),
            render_target_cache_->draw_resolution_scale_x(),
            render_target_cache_->draw_resolution_scale_y())) {
      REXLOG_ERROR("Failed to initialize shader translation");
      return false;
    }
  }
  if (mesh_shader_supported_) {
    uint64_t tess_tables_size = IRRuntimeTessellatorTablesSize();
    tessellator_tables_buffer_ =
        device_->newBuffer(tess_tables_size, MTL::ResourceStorageModeShared);
    if (!tessellator_tables_buffer_) {
      REXLOG_ERROR("Failed to allocate tessellator tables buffer ({} bytes)",
             tess_tables_size);
      return false;
    }
    tessellator_tables_buffer_->setLabel(
        NS::String::string("XeniaTessellatorTables", NS::UTF8StringEncoding));
    IRRuntimeLoadTessellatorTables(tessellator_tables_buffer_);
  }

  // Create the upload buffer pool for per-draw constant buffer allocations.
  constant_buffer_pool_ = std::make_unique<MetalUploadBufferPool>(device_);
  constant_buffer_pool_->SetPageCreatedCallback(
      [this](MTL::Buffer* buffer) { AddResidencySetResource(buffer); });

  // [occlusion-real] FABLE2_OCCLUSION_REAL: visibility-result occlusion queries.
  ZpdInitialize();

  render_encoder_resource_usage_table_.reserve(256);
  render_encoder_heap_usage_.reserve(32);

  // Create a null buffer for unused descriptor entries
  // This prevents shader validation errors when accessing unpopulated
  // descriptors
  null_buffer_ =
      device_->newBuffer(kNullBufferSize, MTL::ResourceStorageModeShared);
  if (!null_buffer_) {
    REXLOG_ERROR("Failed to create null buffer");
    return false;
  }
  null_buffer_->setLabel(
      NS::String::string("NullBuffer", NS::UTF8StringEncoding));
  std::memset(null_buffer_->contents(), 0, kNullBufferSize);

  // Create a 1x1x1 placeholder 2D array texture for unbound texture slots
  // Xbox 360 textures are typically 2D arrays (for texture atlases, cubemaps)
  // Using 2DArray prevents "Invalid texture type" validation errors
  MTL::TextureDescriptor* null_tex_desc =
      MTL::TextureDescriptor::alloc()->init();
  null_tex_desc->setTextureType(MTL::TextureType2DArray);
  null_tex_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  null_tex_desc->setWidth(1);
  null_tex_desc->setHeight(1);
  null_tex_desc->setArrayLength(1);  // Single slice in the array
  null_tex_desc->setStorageMode(MTL::StorageModeShared);
  null_tex_desc->setUsage(MTL::TextureUsageShaderRead);

  null_texture_ = device_->newTexture(null_tex_desc);
  null_tex_desc->release();

  if (!null_texture_) {
    REXLOG_ERROR("Failed to create null texture");
    return false;
  }
  null_texture_->setLabel(
      NS::String::string("NullTexture2DArray", NS::UTF8StringEncoding));

  // Fill the 1x1x1 texture with opaque white (helps debug if sampled)
  uint32_t white_pixel = 0xFFFFFFFF;
  MTL::Region region =
      MTL::Region(0, 0, 0, 1, 1, 1);  // x,y,z origin, w,h,d size
  null_texture_->replaceRegion(region, 0, 0, &white_pixel, 4, 0);  // slice 0

  // Create a default sampler for unbound sampler slots
  // Must set supportsArgumentBuffers=YES for use in argument buffers
  MTL::SamplerDescriptor* null_smp_desc =
      MTL::SamplerDescriptor::alloc()->init();
  null_smp_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMipFilter(MTL::SamplerMipFilterLinear);
  null_smp_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setSupportArgumentBuffers(true);

  null_sampler_ = device_->newSamplerState(null_smp_desc);
  null_smp_desc->release();

  if (!null_sampler_) {
    REXLOG_ERROR("Failed to create null sampler");
    return false;
  }

  system_view_tables_ = device_->newBuffer(
      kSystemViewTableEntryCount * sizeof(IRDescriptorTableEntry),
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!system_view_tables_) {
    REXLOG_ERROR("Failed to create system descriptor tables");
    return false;
  }
  system_view_tables_->setLabel(
      NS::String::string("XeniaSystemViewTables", NS::UTF8StringEncoding));
  std::memset(system_view_tables_->contents(), 0,
              system_view_tables_->length());
  {
    auto* system_entries = reinterpret_cast<IRDescriptorTableEntry*>(
        system_view_tables_->contents());
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    const uint64_t shared_memory_metadata =
        shared_mem_buffer ? shared_mem_buffer->length() : kNullBufferSize;
    IRDescriptorTableSetBuffer(&system_entries[kSystemViewTableSRVSharedMemory],
                               shared_mem_buffer
                                   ? shared_mem_buffer->gpuAddress()
                                   : null_buffer_->gpuAddress(),
                               shared_memory_metadata);
    IRDescriptorTableSetBuffer(&system_entries[kSystemViewTableSRVNull],
                               null_buffer_->gpuAddress(), kNullBufferSize);
    IRDescriptorTableSetBuffer(&system_entries[kSystemViewTableUAVNullStart],
                               null_buffer_->gpuAddress(), kNullBufferSize);
    if (!render_target_cache_ ||
        !render_target_cache_->WriteEdramUintPow2BindlessDescriptor(
            &system_entries[kSystemViewTableUAVNullStart + 1], 2)) {
      REXLOG_ERROR("Failed to encode typed EDRAM UAV system descriptor");
      return false;
    }
    IRDescriptorTableSetBuffer(
        &system_entries[kSystemViewTableUAVSharedMemoryStart],
        shared_mem_buffer ? shared_mem_buffer->gpuAddress()
                          : null_buffer_->gpuAddress(),
        shared_memory_metadata);
    if (!render_target_cache_ ||
        !render_target_cache_->WriteEdramUintPow2BindlessDescriptor(
            &system_entries[kSystemViewTableUAVSharedMemoryStart + 1], 2)) {
      REXLOG_ERROR("Failed to encode typed EDRAM UAV system descriptor");
      return false;
    }
  }
  RegisterInitialResidencySetResources();
  return true;
}

void MetalCommandProcessor::FlushCommandBufferAndWait(uint64_t timeout_ns,
                                                      const char* context) {
  hitch::Scope hitch_scope(hitch::kSync);
  // Phase 1: commit the active command buffer and wait for it.
  if (current_command_buffer_) {
    ++UploadTrace().cbend_wait;
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kCommandBufferEnd);
    if (texture_cache_ &&
        !texture_cache_
             ->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
      REXLOG_ERROR("Metal: failed to flush texture upload encoder before wait");
    }
    uint64_t wait_value = 0;
    if (wait_shared_event_) {
      wait_value = ++wait_shared_event_value_;
      current_command_buffer_->encodeSignalEvent(wait_shared_event_,
                                                 wait_value);
    }
    EncodeMainCommandBufferEnd();
    current_command_buffer_->commit();
    const uint64_t trace_wait_start = gpu_time_trace::Now();
    if (wait_shared_event_) {
      bool signaled =
          wait_shared_event_->waitUntilSignaledValue(wait_value, timeout_ns);
      if (!signaled) {
        REXLOG_ERROR("{}: GPU timeout (possible GPU hang)", context);
      }
    } else {
      current_command_buffer_->waitUntilCompleted();
    }
    gpu_time_trace::RecordWait("tr_sync_wait_us", trace_wait_start);
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    submission_has_draws_ = false;
  }
  DrainCommandBufferAutoreleasePool();

  // Phase 2: submit a dummy command buffer to ensure ALL previously committed
  // GPU work completes before the caller tears down resources.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      const uint64_t trace_wait_start = gpu_time_trace::Now();
      if (wait_shared_event_) {
        bool signaled =
            wait_shared_event_->waitUntilSignaledValue(wait_value, timeout_ns);
        if (!signaled) {
          REXLOG_ERROR("{}: GPU sync timeout (possible GPU hang)", context);
        }
      } else {
        sync_cmd->waitUntilCompleted();
      }
      gpu_time_trace::RecordWait("tr_sync_wait_us", trace_wait_start);
    }
    pool->release();
  }
}

void MetalCommandProcessor::PrepareForWait() {
  if (REXCVAR_GET(metal_async_idle_submission)) {
    // Submit before sleeping so guest polling can make forward progress. Use
    // the normal asynchronous submission path: resource reclamation remains
    // tied to completed submissions and frame slots, not worker idle periods.
    // Explicit readback and shutdown still use FlushCommandBufferAndWait.
    if (current_command_buffer_) ++UploadTrace().cbend_idle;
    EndCommandBuffer();
    CommandProcessor::PrepareForWait();
    gpu_time_trace::idle_start_us = gpu_time_trace::Now();
    if (!hitch_idle_open_ && hitch::Enabled()) {
      hitch::Begin(hitch::kIdle);
      hitch_idle_open_ = true;
    }
    return;
  }
  // Flush pending Metal command buffers before entering wait state so that
  // the worker thread's autorelease pool can drain cleanly.
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kPrepareForWait)) {
    REXLOG_ERROR("Metal PrepareForWait: failed to flush prepared draw queue");
  }
  EndRenderEncoder(RenderEncoderEndReason::kPrepareForWait);
  FlushCommandBufferAndWait(/*timeout_ns=*/5000000000ULL, "PrepareForWait");
  CommandProcessor::PrepareForWait();
  gpu_time_trace::idle_start_us = gpu_time_trace::Now();
  if (!hitch_idle_open_ && hitch::Enabled()) {
    hitch::Begin(hitch::kIdle);
    hitch_idle_open_ = true;
  }
}

void MetalCommandProcessor::ReturnFromWait() {
  if (hitch_idle_open_) {
    hitch::End();
    hitch_idle_open_ = false;
  }
  if (gpu_time_trace::idle_start_us) {
    gpu_time_trace::RecordWait("tr_cp_idle_us", gpu_time_trace::idle_start_us);
    gpu_time_trace::idle_start_us = 0;
  }
  CommandProcessor::ReturnFromWait();
}



void MetalCommandProcessor::WaitForPendingCompletionHandlers() {
  constexpr auto kMaxWait = std::chrono::seconds(5);
  const auto wait_start = std::chrono::steady_clock::now();
  while (pending_completion_handlers_.load(std::memory_order_acquire) != 0) {
    if (std::chrono::steady_clock::now() - wait_start >= kMaxWait) {
      REXLOG_WARN(
          "MetalCommandProcessor: timed out waiting for {} completion "
          "handler(s) during shutdown",
          pending_completion_handlers_.load(std::memory_order_relaxed));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MetalCommandProcessor::ShutdownContext() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kManual)) {
    REXLOG_ERROR("Metal ShutdownContext: failed to flush prepared draw queue");
  }
  MaybeDumpBackendTelemetry("shutdown", true);
  converter_census::MaybeLog(true);

  // End the render encoder directly (not via EndRenderEncoder — we release
  // the encoder object below after the command buffer completes).
  if (current_render_encoder_) {
    UpdateSharedMemoryFenceForActiveRenderEncoder();
    current_render_encoder_->endEncoding();
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kShutdown);

  FlushCommandBufferAndWait(std::numeric_limits<uint64_t>::max(),
                            "ShutdownContext");
  WaitForPendingCompletionHandlers();

  // Now safe to release encoder and command buffer
  if (current_render_encoder_) {
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
    current_render_encoder_has_zpd_visibility_ = false;
    ResetRenderEncoderBufferBindings();
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  DrainCommandBufferAutoreleasePool();
  ShutdownResidencySet();

  constant_buffer_pool_.reset();

  // Shut down texture cache first so texture/sampler destructors can release
  // their bindless heap slots while the heap buffers are still alive.
  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }

  // Release persistent bindless heaps after texture/sampler caches are
  // torn down (destructors write to these heaps during release).
  if (view_bindless_heap_) {
    view_bindless_heap_->release();
    view_bindless_heap_ = nullptr;
  }
  if (sampler_bindless_heap_) {
    sampler_bindless_heap_->release();
    sampler_bindless_heap_ = nullptr;
  }
  if (system_view_tables_) {
    system_view_tables_->release();
    system_view_tables_ = nullptr;
  }
  view_bindless_heap_free_.clear();
  sampler_bindless_heap_free_.clear();
  retired_view_bindless_indices_.clear();
  retired_sampler_bindless_indices_.clear();
  for (RetiredMetalBuffer& retired_buffer : retired_memexport_index_buffers_) {
    if (retired_buffer.buffer) {
      retired_buffer.buffer->release();
    }
  }
  retired_memexport_index_buffers_.clear();
  view_bindless_heap_next_ = 0;
  sampler_bindless_heap_next_ = 0;
  view_bindless_heap_exhausted_logged_ = false;
  sampler_bindless_heap_exhausted_logged_ = false;

  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  if (tessellator_tables_buffer_) {
    tessellator_tables_buffer_->release();
    tessellator_tables_buffer_ = nullptr;
  }
  frame_open_ = false;

  pipeline_cache_.reset();

  // Shut down ZPD query resources.
  ZpdShutdown();

  shared_memory_.reset();
  if (shared_memory_fence_) {
    shared_memory_fence_->release();
    shared_memory_fence_ = nullptr;
  }
  if (wait_shared_event_) {
    wait_shared_event_->release();
    wait_shared_event_ = nullptr;
  }
  if (morph_downscale_pipeline_) {
    morph_downscale_pipeline_->release();
    morph_downscale_pipeline_ = nullptr;
  }
  if (morph_downscale_buffer_) {
    morph_downscale_buffer_->release();
    morph_downscale_buffer_ = nullptr;
  }

  CommandProcessor::ShutdownContext();
}

uint32_t MetalCommandProcessor::AllocateViewBindlessIndex() {
  if (!view_bindless_heap_free_.empty()) {
    uint32_t idx = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    view_bindless_heap_exhausted_logged_ = false;
    return idx;
  }
  if (view_bindless_heap_next_ >= kViewBindlessHeapSize) {
    // Reclaim retired indices from completed submissions first — this is cheap
    // and may free slots without needing to evict any textures.
    ProcessCompletedSubmissions();
    if (!view_bindless_heap_free_.empty()) {
      uint32_t idx = view_bindless_heap_free_.back();
      view_bindless_heap_free_.pop_back();
      view_bindless_heap_exhausted_logged_ = false;
      return idx;
    }
    // Still full — try evicting least-recently-used textures.
    if (texture_cache_ && texture_cache_->TrimViewBindlessPressure()) {
      if (!view_bindless_heap_free_.empty()) {
        uint32_t idx = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
        view_bindless_heap_exhausted_logged_ = false;
        return idx;
      }
    }
    if (!view_bindless_heap_exhausted_logged_) {
      uint64_t completed =
          completed_command_buffers_.load(std::memory_order_relaxed);
      uint64_t texture_count =
          texture_cache_ ? texture_cache_->GetTotalTextureCount() : 0;
      REXLOG_ERROR(
          "View bindless heap exhausted ({} allocated, {} free, {} retired, "
          "submissions: current={} completed={}, textures={})",
          view_bindless_heap_next_, view_bindless_heap_free_.size(),
          retired_view_bindless_indices_.size(), submission_current_, completed,
          texture_count);
      view_bindless_heap_exhausted_logged_ = true;
    }
    return UINT32_MAX;
  }
  return view_bindless_heap_next_++;
}

void MetalCommandProcessor::ReleaseViewBindlessIndex(uint32_t index) {
  if (index >= kViewBindlessHeapSize || !view_bindless_heap_) {
    return;
  }
  // Match D3D12's persistent texture-descriptor lifetime more closely: by the
  // time a Metal texture reaches destruction through the cache, its last GPU
  // use has already completed, so the bindless view slot can be recycled
  // immediately rather than waiting for the current submission to end.
  FreeViewBindlessIndexNow(index);
}

void MetalCommandProcessor::RetireViewBindlessIndex(uint32_t index) {
  if (index >= kViewBindlessHeapSize || !view_bindless_heap_) {
    return;
  }
  uint64_t retirement_submission = GetBindlessDescriptorRetirementSubmission();
  if (!retirement_submission) {
    FreeViewBindlessIndexNow(index);
  } else {
    retired_view_bindless_indices_.push_back({index, retirement_submission});
  }
}

uint32_t MetalCommandProcessor::GetViewBindlessHeapAvailableCount() const {
  return uint32_t(kViewBindlessHeapSize - view_bindless_heap_next_) +
         uint32_t(view_bindless_heap_free_.size());
}

uint32_t MetalCommandProcessor::AllocateSamplerBindlessIndex() {
  if (!sampler_bindless_heap_free_.empty()) {
    uint32_t idx = sampler_bindless_heap_free_.back();
    sampler_bindless_heap_free_.pop_back();
    sampler_bindless_heap_exhausted_logged_ = false;
    return idx;
  }
  if (sampler_bindless_heap_next_ >= kSamplerBindlessHeapSize) {
    if (!sampler_bindless_heap_exhausted_logged_) {
      REXLOG_ERROR("Sampler bindless heap exhausted");
      sampler_bindless_heap_exhausted_logged_ = true;
    }
    return UINT32_MAX;
  }
  return sampler_bindless_heap_next_++;
}

void MetalCommandProcessor::ReleaseSamplerBindlessIndex(uint32_t index) {
  if (index >= kSamplerBindlessHeapSize || !sampler_bindless_heap_) {
    return;
  }
  uint64_t retirement_submission = GetBindlessDescriptorRetirementSubmission();
  if (!retirement_submission) {
    FreeSamplerBindlessIndexNow(index);
  } else {
    retired_sampler_bindless_indices_.push_back({index, retirement_submission});
  }
}

uint64_t MetalCommandProcessor::GetBindlessDescriptorRetirementSubmission()
    const {
  if (!submission_current_) {
    return 0;
  }
  if (current_command_buffer_ ||
      completed_command_buffers_.load(std::memory_order_relaxed) <
          submission_current_) {
    return submission_current_;
  }
  return 0;
}

void MetalCommandProcessor::FreeViewBindlessIndexNow(uint32_t index) {
  if (index >= kViewBindlessHeapSize || !view_bindless_heap_) {
    return;
  }
  if (IRDescriptorTableEntry* entry = GetViewBindlessHeapEntry(index)) {
    std::memset(entry, 0, sizeof(IRDescriptorTableEntry));
  }
  view_bindless_heap_free_.push_back(index);
  view_bindless_heap_exhausted_logged_ = false;
}

void MetalCommandProcessor::FreeSamplerBindlessIndexNow(uint32_t index) {
  if (index >= kSamplerBindlessHeapSize || !sampler_bindless_heap_) {
    return;
  }
  if (IRDescriptorTableEntry* entry = GetSamplerBindlessHeapEntry(index)) {
    std::memset(entry, 0, sizeof(IRDescriptorTableEntry));
  }
  sampler_bindless_heap_free_.push_back(index);
  sampler_bindless_heap_exhausted_logged_ = false;
}

IRDescriptorTableEntry* MetalCommandProcessor::GetViewBindlessHeapEntry(
    uint32_t index) {
  if (!view_bindless_heap_) {
    REXLOG_ERROR("GetViewBindlessHeapEntry: heap is null!");
    return nullptr;
  }
  if (index >= kViewBindlessHeapSize) {
    REXLOG_ERROR("GetViewBindlessHeapEntry: index {} >= heap size {}", index,
           kViewBindlessHeapSize);
    return nullptr;
  }
  return reinterpret_cast<IRDescriptorTableEntry*>(
             view_bindless_heap_->contents()) +
         index;
}

IRDescriptorTableEntry* MetalCommandProcessor::GetSamplerBindlessHeapEntry(
    uint32_t index) {
  if (!sampler_bindless_heap_) {
    REXLOG_ERROR("GetSamplerBindlessHeapEntry: heap is null!");
    return nullptr;
  }
  if (index >= kSamplerBindlessHeapSize) {
    REXLOG_ERROR("GetSamplerBindlessHeapEntry: index {} >= heap size {}", index,
           kSamplerBindlessHeapSize);
    return nullptr;
  }
  return reinterpret_cast<IRDescriptorTableEntry*>(
             sampler_bindless_heap_->contents()) +
         index;
}

void MetalCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  if (pipeline_cache_) {
    // [xfer-prewarm] Start before the shader storage init: on a first launch
    // (empty Metal system cache) the cached translations take seconds to
    // install, and the transfer pipelines build beside them instead of after.
    // FABLE2_XFER_PREWARM_EARLY=0: after storage init, as before.
    static const bool early = [] {
      const char* v = std::getenv("FABLE2_XFER_PREWARM_EARLY");
      return !(v && v[0] == '0');
    }();
    if (early && render_target_cache_) {
      render_target_cache_->StartTransferPrewarm(
          pipeline_cache_->ShaderStorageTitleRootFor(cache_root, title_id),
          title_id);
    }
    if (render_target_cache_) {
      // [tile-resolve prewarm] beside the transfer prewarm.
      render_target_cache_->StartTileResolvePrewarm(
          pipeline_cache_->ShaderStorageTitleRootFor(cache_root, title_id),
          title_id);
    }
    pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
    if (!early && render_target_cache_ &&
        !pipeline_cache_->shader_storage_title_root().empty()) {
      render_target_cache_->StartTransferPrewarm(
          pipeline_cache_->shader_storage_title_root(), title_id);
    }
  }
}

// [rt-shrink] FABLE2_MEM_REPORT=<seconds>: every N seconds (at a swap) log the
// process footprint and the main Metal allocations. Log only, no GPU work.
void MetalCommandProcessor::MaybeLogMemReport() {
  static const double interval = [] {
    const char* v = std::getenv("FABLE2_MEM_REPORT");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (interval <= 0.0) return;
  static auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - last).count() < interval) return;
  last = now;
  constexpr double kMB = 1024.0 * 1024.0;
  task_vm_info_data_t vm{};
  mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
  uint64_t footprint = 0;
  if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&vm), &vm_count) ==
      KERN_SUCCESS) {
    footprint = vm.phys_footprint;
  }
  uint64_t device_bytes = device_ ? uint64_t(device_->currentAllocatedSize()) : 0;
  MetalRenderTargetCache::MemReportInfo rt;
  if (render_target_cache_) render_target_cache_->GetMemReportInfo(rt);
  MetalTextureCache::MemReportInfo tx;
  if (texture_cache_) texture_cache_->GetMemReportInfo(tx);
  uint64_t shared_bytes = 0;
  if (shared_memory_ && shared_memory_->GetBuffer()) {
    shared_bytes = uint64_t(shared_memory_->GetBuffer()->allocatedSize());
  }
  REXLOG_INFO(
      "[mem-report] f{} footprint {:.0f}MB device {:.0f}MB | RT {} = {:.0f}MB "
      "(full-height {:.0f}MB) heaps {} = {:.0f}MB used {:.0f}MB | edram "
      "{:.0f}MB rt-other {:.0f}MB | textures {} = {:.0f}MB (tex heaps {} = "
      "{:.0f}MB used {:.0f}MB, staging {:.0f}MB) | scaled {} = "
      "{:.0f}MB retired {} = {:.0f}MB | shared {:.0f}MB",
      frame_log::CurrentFrame(), footprint / kMB, device_bytes / kMB,
      rt.rt_count, rt.rt_bytes / kMB, rt.rt_full_bytes / kMB, rt.heap_count,
      rt.heap_bytes / kMB, rt.heap_used_bytes / kMB, rt.edram_bytes / kMB,
      rt.other_bytes / kMB, tx.tex_count, tx.tex_bytes / kMB, tx.heap_count,
      tx.heap_bytes / kMB, tx.heap_used_bytes / kMB, tx.staging_bytes / kMB,
      tx.scaled_count,
      tx.scaled_bytes / kMB, tx.retired_count, tx.retired_bytes / kMB,
      shared_bytes / kMB);
  REXLOG_INFO(
      "[mem-report] RT rows-needed estimate {:.0f}MB, grows {} ({:.0f}MB "
      "copied), verify misses {} (unclipped draws {}, unowned claims {})",
      rt.rt_needed_bytes / kMB, rt.grows, rt.grow_bytes / kMB, rt.misses,
      rt.unclipped_draws, rt.unowned_claims);
  REXLOG_INFO("[mem-report] top RTs: {}", rt.top);
}

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  ZpdMaybeLogStats();
  if (hle_census::Enabled()) {
    hle_census::EndFrame(frame_log::CurrentFrame());
  }
  gpu_time_trace::SampleThreadCpu();
  {
    // [ready] before hitch::OnSwap resets the per-frame scope counts.
    // Draw-path pipeline / translation time this frame (the scopes are also entered on cache hits, so time,
    // not counts).
    const hitch::ThreadState& hs = hitch::State();
    uint64_t pso_ticks = 0;
    for (int c = hitch::kPsoWait; c <= hitch::kXlate; ++c) pso_ticks += hs.ticks[c];
    readiness::OnSwap(frame_log::CurrentFrame(), hitch::TicksToUs(pso_ticks));
  }
  hitch::OnSwap(frame_log::CurrentFrame());
  converter_census::MaybeLog();
  frame_log::BeginFrame();
  ProcessCompletedSubmissions();
  MaybeLogMemReport();
  saw_swap_ = true;
  ++backend_telemetry_.swaps;
  last_swap_ptr_ = frontbuffer_ptr;
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;

  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kSwap)) {
    REXLOG_ERROR("Metal IssueSwap: failed to flush prepared draw queue");
  }

  // End any active render encoder
  EndRenderEncoder(RenderEncoderEndReason::kSwap);
  EndSharedMemoryUploadBlitEncoder(SharedMemoryUploadEncoderEndReason::kSwap);
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    REXLOG_ERROR("Metal: failed to flush texture upload encoder before swap");
  }

  // Submit and wait for command buffer
  if (current_command_buffer_) {
    EncodeMainCommandBufferEnd();
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    submission_has_draws_ = false;
  }
  DrainCommandBufferAutoreleasePool();

  CloseFrameLifetime();
  // Proactive descriptor-pressure trimming: at frame boundaries the GPU has
  // likely completed prior submissions, so retired descriptor indices can be
  // reclaimed and the texture cache can be trimmed before we start the next
  // frame under pressure.  The high-water mark (75% utilization) gives
  // headroom so that mid-frame allocation bursts don't immediately exhaust
  // the heap.
  if (texture_cache_) {
    constexpr uint32_t kDescriptorPressureThreshold =
        kViewBindlessHeapSize / 4;  // trim when < 25% free
    uint32_t available = GetViewBindlessHeapAvailableCount();
    if (available < kDescriptorPressureThreshold) {
      texture_cache_->TrimViewBindlessPressure(kDescriptorPressureThreshold);
    }
  }

  if (shared_memory_ && REXCVAR_GET(clear_memory_page_state)) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }

  // Push the rendered frame to the presenter's guest output mailbox.
  // This is required for trace dumps to capture the output via the
  // MetalRenderTargetCache color target (like D3D12).
  auto* presenter =
      static_cast<ui::metal::MetalPresenter*>(graphics_system_->presenter());
  if (presenter && render_target_cache_) {
    uint32_t output_width = frontbuffer_width ? frontbuffer_width : 1280;
    uint32_t output_height = frontbuffer_height ? frontbuffer_height : 720;

    MTL::Texture* source_texture = nullptr;
    bool use_pwl_gamma_ramp = false;
    if (texture_cache_) {
      uint32_t swap_width = 0;
      uint32_t swap_height = 0;
      xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
      source_texture = texture_cache_->RequestSwapTexture(
          swap_width, swap_height, swap_format);
      if (source_texture) {
        output_width = swap_width;
        output_height = swap_height;
        use_pwl_gamma_ramp =
            swap_format == xenos::TextureFormat::k_2_10_10_10 ||
            swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        if (presenter) {
          if (!gamma_ramp_256_entry_table_up_to_date_ ||
              !gamma_ramp_pwl_up_to_date_) {
            constexpr size_t kGammaRampTableBytes =
                sizeof(reg::DC_LUT_30_COLOR) * 256;
            constexpr size_t kGammaRampPwlBytes =
                sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3;
            if (presenter->UpdateGammaRamp(
                    gamma_ramp_256_entry_table(), kGammaRampTableBytes,
                    gamma_ramp_pwl_rgb(), kGammaRampPwlBytes)) {
              gamma_ramp_256_entry_table_up_to_date_ = true;
              gamma_ramp_pwl_up_to_date_ = true;
            } else {
              REXLOG_WARN("Metal IssueSwap: gamma ramp upload failed");
            }
          }
        }
      }
    }

    bool swap_dest_swap = false;
    const bool has_swap_dest_swap =
        ConsumeSwapDestSwap(frontbuffer_ptr, &swap_dest_swap);
    bool force_swap_rb = has_swap_dest_swap && swap_dest_swap;

    if (!source_texture) {
      static bool missing_swap_logged = false;
      if (!missing_swap_logged) {
        missing_swap_logged = true;
        REXLOG_WARN(
            "MetalCommandProcessor::IssueSwap: swap texture unavailable; "
            "presenting inactive (black) output");
      }
      presenter->RefreshGuestOutput(
          0, 0, 0, 0, [](ui::Presenter::GuestOutputRefreshContext&) -> bool {
            return false;
          });
      MaybeDumpBackendTelemetry("swap");
      return;
    }

    if (source_texture) {
      ui::metal::MetalPresenter* metal_presenter = presenter;
      uint32_t source_width = output_width;
      uint32_t source_height = output_height;
      bool force_swap_rb_copy = force_swap_rb;
      bool use_pwl_gamma_ramp_copy = use_pwl_gamma_ramp;
      // Match ReXGlue's presenter aspect ratio to the guest video mode.
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      const std::pair<uint32_t, uint32_t> aspect{
          std::max(1u, uint32_t(video_mode.display_width)),
          std::max(1u, uint32_t(video_mode.display_height))};
      hitch::Scope hitch_present_scope(hitch::kPresent);
      presenter->RefreshGuestOutput(
          output_width, output_height, aspect.first, aspect.second,
          [source_texture, metal_presenter, source_width, source_height,
           force_swap_rb_copy, use_pwl_gamma_ramp_copy](
              ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& metal_context =
                static_cast<ui::metal::MetalGuestOutputRefreshContext&>(
                    context);
            context.SetIs8bpc(!use_pwl_gamma_ramp_copy);
            uint64_t submission_id = 0;
            bool copy_success = metal_presenter->CopyTextureToGuestOutput(
                source_texture, metal_context.resource_uav_capable(),
                source_width, source_height, force_swap_rb_copy,
                use_pwl_gamma_ramp_copy, &submission_id);
            if (copy_success && submission_id) {
              metal_context.SetSubmissionId(submission_id);
            }
            return copy_success;
          });
    }
  }
  if (REXCVAR_GET(metal_probe_capture_interval) > 0) {
    ++diagnostic_swap_count_;
    auto interval = uint32_t(REXCVAR_GET(metal_probe_capture_interval));
    if (diagnostic_swap_count_ % interval == 0 &&
        diagnostic_swap_count_ / interval <= 6) {
      ui::RawImage image;
      if (auto* presenter = graphics_system_->presenter();
          presenter && presenter->CaptureGuestOutput(image)) {
        std::vector<uint8_t> rgb(size_t(image.width) * image.height * 3);
        for (uint32_t y = 0; y < image.height; ++y) {
          for (uint32_t x = 0; x < image.width; ++x) {
            std::memcpy(rgb.data() + (size_t(y) * image.width + x) * 3,
                        image.data.data() + y * image.stride + x * 4, 3);
          }
        }
        auto filename = fmt::format("metal-frame-{:08}.ppm", diagnostic_swap_count_);
        std::ofstream output(filename, std::ios::binary);
        output << "P6\n" << image.width << " " << image.height << "\n255\n";
        output.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        REXLOG_INFO("Metal diagnostic frame captured: {}", filename);
      }
    }
  }
  MaybeDumpBackendTelemetry("swap");
  GpuCaptureTick(command_queue_);
}

void MetalCommandProcessor::OnPrimaryBufferEnd() {
  // [ring-wait] The read-pointer writeback follows as soon as this returns.
  struct NotifyOnExit {
    ~NotifyOnExit() {
      if (!RingNotifyAfterStore()) {
        NotifyRingProgress();
      }
    }
  } notify_on_exit;
  if (!current_command_buffer_) {
    return;
  }

  // Pump any completed resolves now since the guest is likely about to poll.

  if (!REXCVAR_GET(submit_on_primary_buffer_end)) {
    return;
  }

  if (!CanEndSubmissionImmediately()) {
    return;
  }
  ++UploadTrace().cbend_pbe;
  EndCommandBuffer();
}


bool MetalCommandProcessor::CanEndSubmissionImmediately() {
  if (!current_command_buffer_) {
    return false;
  }
  if (pipeline_cache_ && pipeline_cache_->IsCreatingPipelines()) {
    return false;
  }
  return true;
}

// ============================================================================
// ZPD (occlusion query) backend overrides.
// ============================================================================

// [occlusion-real] real queries on Metal visibility results (metal_zpd_real.cc,
// default on). FABLE2_OCCLUSION_REAL=0 falls back to the ReXGlue PM4 path:
// every finished query reads query_occlusion_fake_sample_count (1000) samples.
// [zpd-stats] FABLE2_ZPD_STATS=1: count the game's query begins/ends and log a
// summary every 600 calls, so the audit can tell whether Fable II uses them.
bool MetalCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(
    memory::RingBuffer* reader, uint32_t packet, uint32_t count) {
  static const bool stats = [] {
    const char* v = std::getenv("FABLE2_ZPD_STATS");
    return v && v[0] == '1';
  }();
  if (stats) {
    static uint64_t begins = 0, ends = 0, no_addr = 0;
    const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
    auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!counts) {
      ++no_addr;
    } else if (counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished ||
               counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished) {
      ++ends;
    } else {
      ++begins;
    }
    static uint64_t last_log_frame = 0;
    if (begins + ends + no_addr == 1 || frame_current_ >= last_log_frame + 600) {
      last_log_frame = frame_current_;
      REXLOG_INFO("[zpd-stats] frame {}: {} begins, {} ends, {} without a sample address",
                  frame_current_, begins, ends, no_addr);
    }
  }
  // FABLE2_ZPD_DIAG=zero (diagnostic only): finished queries report 0 passed
  // samples, so the audit can see what the game gates on its queries.
  static const bool zero = [] {
    const char* v = std::getenv("FABLE2_ZPD_DIAG");
    return v && std::strcmp(v, "zero") == 0;
  }();
  if (zpd_real_) {
    return ZpdExecuteEvent(reader, packet, count);
  }
  bool end_query = false;
  if (zero) {
    const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
    if (auto* c = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
            register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR])) {
      end_query = c->ZPass_A == kQueryFinished || c->ZPass_B == kQueryFinished ||
                  c->ZFail_A == kQueryFinished || c->ZFail_B == kQueryFinished;
    }
  }
  const bool result =
      CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  if (zero && end_query) {
    if (auto* c = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
            register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR])) {
      c->ZPass_A = 0;
      c->Total_A = 0;
    }
  }
  return result;
}

void MetalCommandProcessor::MarkSharedMemoryComputeWritePending(
    uint32_t address, uint32_t length, MTL::ComputeCommandEncoder* encoder) {
  if (!length) {
    return;
  }
  if (shared_memory_fence_ && encoder) {
    encoder->updateFence(shared_memory_fence_);
    MarkSharedMemoryWritePending(address, length, MTL::RenderStages(0), false,
                                 true);
    return;
  }
  REXLOG_ERROR("MetalCommandProcessor: shared-memory compute write cannot be fenced");
  MarkSharedMemoryWritePending(address, length, MTL::RenderStages(0), false,
                               false);
}

void MetalCommandProcessor::MarkSharedMemoryRenderWritePending(
    uint32_t address, uint32_t length, MTL::RenderStages stages) {
  if (!length) {
    return;
  }
  if (current_render_encoder_ && NS::UInteger(stages)) {
    active_render_encoder_shared_memory_write_stages_ = MTL::RenderStages(
        NS::UInteger(active_render_encoder_shared_memory_write_stages_) |
        NS::UInteger(stages));
    MarkSharedMemoryWritePending(address, length, stages, true, false);
    return;
  }
  REXLOG_ERROR("MetalCommandProcessor: shared-memory render write cannot be fenced");
  MarkSharedMemoryWritePending(address, length, MTL::RenderStages(0), false,
                               false);
}

bool MetalCommandProcessor::PrepareSharedMemoryComputeReadDependency(
    const SharedMemoryRange* ranges, uint32_t range_count,
    bool consumer_can_join_current_submission,
    SharedMemoryReadDependency* dependency_out) {
  if (dependency_out) {
    *dependency_out = {};
  }
  if (!ranges || !range_count ||
      !PendingSharedMemoryWritesOverlapRanges(ranges, range_count)) {
    return true;
  }

  if (current_render_encoder_) {
    EndRenderEncoder(RenderEncoderEndReason::kSharedMemoryReadDependency);
  }

  bool needs_fence_wait = false;
  bool needs_submission_boundary = false;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (!PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      continue;
    }
    if (pending.fence_updated && shared_memory_fence_) {
      needs_fence_wait = true;
      continue;
    }
    needs_submission_boundary = true;
    break;
  }

  // Ending the render encoder above may make transfer work eligible to join the
  // producer command buffer. Only skip a submission boundary when the consumer
  // can actually encode into that ordered submission; standalone consumers
  // still carry the fence wait after the producer is committed.
  const bool consumer_in_current_submission =
      consumer_can_join_current_submission &&
      CanJoinActiveSubmissionForTransfer();
  if (needs_fence_wait && dependency_out) {
    dependency_out->needs_fence_wait = true;
  }

  if (current_command_buffer_ &&
      (needs_submission_boundary ||
       (needs_fence_wait && !consumer_in_current_submission))) {
    ++UploadTrace().cbend_dep;
    EndCommandBuffer();
  }
  return true;
}

bool MetalCommandProcessor::EncodeSharedMemoryComputeReadDependency(
    MTL::ComputeCommandEncoder* encoder,
    const SharedMemoryReadDependency& dependency,
    const SharedMemoryRange* ranges, uint32_t range_count) {
  if (!dependency.needs_fence_wait) {
    return true;
  }
  if (!encoder || !shared_memory_fence_) {
    REXLOG_ERROR(
        "MetalCommandProcessor: compute read cannot wait for shared-memory GPU "
        "writes without a fence");
    return false;
  }

  encoder->waitForFence(shared_memory_fence_);
  RetireFenceWaitedSharedMemoryWrites(ranges, range_count);
  return true;
}

void MetalCommandProcessor::MarkSharedMemoryWritePending(
    uint32_t address, uint32_t length, MTL::RenderStages producer_stages,
    bool active_render_encoder, bool fence_updated) {
  if (!length) {
    return;
  }

  if (active_render_encoder) ++shared_memory_active_write_gen_;
  if (fence_updated) ++shared_memory_fence_update_gen_;
  constexpr uint32_t kSharedMemoryMask = SharedMemory::kBufferSize - 1;
  uint32_t remaining = std::min(length, SharedMemory::kBufferSize);
  uint32_t start = address & kSharedMemoryMask;
  while (remaining) {
    uint32_t segment_length =
        std::min(remaining, SharedMemory::kBufferSize - start);
    if (segment_length) {
      PendingSharedMemoryWrite pending = {};
      pending.start = start;
      pending.end = start + segment_length;
      pending.submission_id = GetCurrentSubmission();
      pending.producer_stages = producer_stages;
      pending.active_render_encoder = active_render_encoder;
      pending.fence_updated = fence_updated;
      pending_shared_memory_writes_.push_back(pending);
      if (shared_memory_) {
        // [texture-precommit] exact per-page GPU write record: unlike this
        // list it is never collapsed to the whole buffer.
        shared_memory_->MarkGpuWrite(start, segment_length,
                                     GetCurrentSubmission());
      }
    }
    remaining -= segment_length;
    start = 0;
  }

  if (pending_shared_memory_writes_.size() <= kMaxPendingSharedMemoryWrites) {
    return;
  }
  ++gpu_time_trace::n_collapse;

  PendingSharedMemoryWrite collapsed = {};
  collapsed.start = 0;
  collapsed.end = SharedMemory::kBufferSize;
  collapsed.submission_id = GetCurrentSubmission();
  collapsed.fence_updated = true;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    collapsed.submission_id =
        std::max(collapsed.submission_id, pending.submission_id);
    collapsed.producer_stages =
        MTL::RenderStages(NS::UInteger(collapsed.producer_stages) |
                          NS::UInteger(pending.producer_stages));
    collapsed.active_render_encoder =
        collapsed.active_render_encoder || pending.active_render_encoder;
    collapsed.fence_updated = collapsed.fence_updated && pending.fence_updated;
  }
  if (collapsed.active_render_encoder) {
    collapsed.fence_updated = false;
  }
  pending_shared_memory_writes_.assign(1, collapsed);
}

bool MetalCommandProcessor::PendingSharedMemoryWritesOverlapRange(
    uint32_t start, uint32_t length) const {
  SharedMemoryRange range = {start, length};
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (PendingSharedMemoryWriteOverlapsRanges(pending, &range, 1)) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::PendingSharedMemoryWriteOverlapsRanges(
    const PendingSharedMemoryWrite& pending, const SharedMemoryRange* ranges,
    uint32_t range_count) const {
  if (!ranges || !range_count || pending.end <= pending.start) {
    return false;
  }
  for (uint32_t i = 0; i < range_count; ++i) {
    if (SharedMemoryRangeOverlapsSegment(ranges[i], pending.start,
                                         pending.end)) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::PendingSharedMemoryWritesOverlapRanges(
    const SharedMemoryRange* ranges, uint32_t range_count) const {
  if (!ranges || !range_count || pending_shared_memory_writes_.empty()) {
    return false;
  }
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::UpdateSharedMemoryFenceForActiveRenderEncoder() {
  if (!current_render_encoder_ ||
      !NS::UInteger(active_render_encoder_shared_memory_write_stages_)) {
    return;
  }

  if (shared_memory_fence_) {
    current_render_encoder_->updateFence(
        shared_memory_fence_,
        active_render_encoder_shared_memory_write_stages_);
  }
  ++shared_memory_fence_update_gen_;

  for (PendingSharedMemoryWrite& pending : pending_shared_memory_writes_) {
    if (!pending.active_render_encoder) {
      continue;
    }
    pending.active_render_encoder = false;
    pending.fence_updated = shared_memory_fence_ != nullptr;
  }
  active_render_encoder_shared_memory_write_stages_ = MTL::RenderStages(0);
}

void MetalCommandProcessor::PruneCompletedSharedMemoryWrites(
    uint64_t completed_submission) {
  pending_shared_memory_writes_.erase(
      std::remove_if(
          pending_shared_memory_writes_.begin(),
          pending_shared_memory_writes_.end(),
          [completed_submission](const PendingSharedMemoryWrite& pending) {
            return !pending.active_render_encoder &&
                   pending.submission_id <= completed_submission;
          }),
      pending_shared_memory_writes_.end());
}

void MetalCommandProcessor::RetireFenceWaitedSharedMemoryWrites(
    const SharedMemoryRange* ranges, uint32_t range_count) {
  if (!ranges || !range_count) {
    return;
  }
  // [draw-prep-lite] Nothing to retire (no fence-waited entry overlaps) and no
  // collapse due: the pass below would rebuild the same list.
  if (DrawPrepLiteEnabled() &&
      pending_shared_memory_writes_.size() <= kMaxPendingSharedMemoryWrites) {
    bool any = false;
    for (const PendingSharedMemoryWrite& pending :
         pending_shared_memory_writes_) {
      if (!pending.active_render_encoder && pending.fence_updated &&
          PendingSharedMemoryWriteOverlapsRanges(pending, ranges,
                                                 range_count)) {
        any = true;
        break;
      }
    }
    if (!any) {
      return;
    }
  }
  // [draw-prep-lite2] build the retained list in a reused member vector (same
  // capacity limit) and swap it in, instead of a value-initialized 64-entry stack
  // array copied back with assign().
  const bool lite2 = DrawPrepLite2Enabled();
  std::optional<
      std::array<PendingSharedMemoryWrite, kMaxPendingSharedMemoryWriteCapacity>>
      retained_entries_storage;
  if (!lite2) {
    retained_entries_storage.emplace();
  }
  std::vector<PendingSharedMemoryWrite>& retained_vector =
      pending_shared_memory_writes_scratch_;
  if (lite2) {
    retained_vector.clear();
  }
  size_t retained_count = 0;
  bool retained_overflow = false;
  auto retain_entry = [&](const PendingSharedMemoryWrite& pending) {
    if (retained_count >= kMaxPendingSharedMemoryWriteCapacity) {
      retained_overflow = true;
      return;
    }
    if (lite2) {
      retained_vector.push_back(pending);
      ++retained_count;
    } else {
      (*retained_entries_storage)[retained_count++] = pending;
    }
  };

  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (pending.active_render_encoder || !pending.fence_updated ||
        !PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      retain_entry(pending);
      continue;
    }

    std::array<SharedMemoryRangeSegment, kMaxSharedMemoryWaitSegmentsPerPending>
        remaining_segments;
    std::array<SharedMemoryRangeSegment, kMaxSharedMemoryWaitSegmentsPerPending>
        next_segments;
    size_t remaining_count = 1;
    remaining_segments[0] = {pending.start, pending.end};
    bool segment_overflow = false;

    for (uint32_t i = 0; i < range_count && remaining_count; ++i) {
      ForEachSharedMemoryRangeSegment(
          ranges[i], [&](SharedMemoryRangeSegment waited_segment) {
            if (segment_overflow || !remaining_count) {
              return;
            }
            size_t next_count = 0;
            auto append_next = [&](SharedMemoryRangeSegment segment) {
              if (next_count >= next_segments.size()) {
                segment_overflow = true;
                return;
              }
              next_segments[next_count++] = segment;
            };

            for (size_t segment_index = 0; segment_index < remaining_count;
                 ++segment_index) {
              const SharedMemoryRangeSegment segment =
                  remaining_segments[segment_index];
              if (waited_segment.start >= segment.end ||
                  waited_segment.end <= segment.start) {
                append_next(segment);
                continue;
              }
              if (segment.start < waited_segment.start) {
                append_next({segment.start, waited_segment.start});
              }
              if (waited_segment.end < segment.end) {
                append_next({waited_segment.end, segment.end});
              }
            }

            if (!segment_overflow) {
              for (size_t segment_index = 0; segment_index < next_count;
                   ++segment_index) {
                remaining_segments[segment_index] =
                    next_segments[segment_index];
              }
              remaining_count = next_count;
            }
          });
      if (segment_overflow) {
        break;
      }
    }

    if (segment_overflow) {
      // Preserve correctness if the range set fragments too much for the fixed
      // stack workspace. This may cause one extra future wait, but avoids heap
      // churn and never drops an unwaited byte range.
      retain_entry(pending);
      continue;
    }

    for (size_t segment_index = 0; segment_index < remaining_count;
         ++segment_index) {
      const SharedMemoryRangeSegment segment =
          remaining_segments[segment_index];
      if (segment.start >= segment.end) {
        continue;
      }
      PendingSharedMemoryWrite split_pending = pending;
      split_pending.start = segment.start;
      split_pending.end = segment.end;
      retain_entry(split_pending);
    }
  }

  if (retained_overflow) {
    return;
  }

  if (lite2) {
    pending_shared_memory_writes_.swap(retained_vector);
  } else {
    pending_shared_memory_writes_.assign(
        retained_entries_storage->begin(),
        retained_entries_storage->begin() + retained_count);
  }
  if (pending_shared_memory_writes_.size() <= kMaxPendingSharedMemoryWrites) {
    return;
  }
  ++gpu_time_trace::n_collapse;

  PendingSharedMemoryWrite collapsed = {};
  collapsed.start = 0;
  collapsed.end = SharedMemory::kBufferSize;
  collapsed.submission_id = GetCurrentSubmission();
  collapsed.fence_updated = true;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    collapsed.submission_id =
        std::max(collapsed.submission_id, pending.submission_id);
    collapsed.producer_stages =
        MTL::RenderStages(NS::UInteger(collapsed.producer_stages) |
                          NS::UInteger(pending.producer_stages));
    collapsed.active_render_encoder =
        collapsed.active_render_encoder || pending.active_render_encoder;
    collapsed.fence_updated = collapsed.fence_updated && pending.fence_updated;
  }
  if (collapsed.active_render_encoder) {
    collapsed.fence_updated = false;
  }
  pending_shared_memory_writes_.assign(1, collapsed);
}

bool MetalCommandProcessor::EncodeSharedMemoryRenderReadDependencies(
    const SharedMemoryRange* ranges, uint32_t range_count,
    MTL::RenderStages consumer_stages) {
  if (!current_render_encoder_ || !ranges || !range_count ||
      !NS::UInteger(consumer_stages) || pending_shared_memory_writes_.empty()) {
    return true;
  }

  MTL::RenderStages active_producer_stages = MTL::RenderStages(0);
  bool needs_fence_wait = false;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (!PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      continue;
    }
    if (pending.active_render_encoder) {
      active_producer_stages =
          MTL::RenderStages(NS::UInteger(active_producer_stages) |
                            NS::UInteger(pending.producer_stages));
    } else if (pending.fence_updated && shared_memory_fence_) {
      needs_fence_wait = true;
    } else {
      REXLOG_ERROR(
          "MetalCommandProcessor: render read cannot wait for shared-memory "
          "GPU "
          "writes without a fence");
      return false;
    }
  }

  // FABLE2_RENDER_BARRIER_DEDUP (default on, =0 off; docs/weak-mac-perf-20261008.md):
  // a barrier orders every earlier command's producer stages before every later
  // command's consumer stages, and a fence wait covers every update encoded
  // before it. So once this encoder has a barrier (or wait) for the current
  // producer generation and stages, later identical ones are redundant. Without
  // this, a pending range that overlaps everything (the 16-entry list collapses
  // to the whole buffer) puts a barrier/fence wait before every draw.
  static const bool dedup = [] {
    const char* v = std::getenv("FABLE2_RENDER_BARRIER_DEDUP");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO("[barrier-dedup] {} (FABLE2_RENDER_BARRIER_DEDUP={})",
                on ? "on: redundant render-encoder barriers/fence waits skipped"
                   : "off",
                v ? v : "unset");
    return on;
  }();
  const NS::UInteger consumers = NS::UInteger(consumer_stages);
  if (NS::UInteger(active_producer_stages)) {
    const NS::UInteger producers = NS::UInteger(active_producer_stages);
    if (dedup && render_barrier_gen_ == shared_memory_active_write_gen_ &&
        (render_barrier_producers_ & producers) == producers &&
        (render_barrier_consumers_ & consumers) == consumers) {
      ++gpu_time_trace::n_barrier_skipped;
    } else {
      current_render_encoder_->memoryBarrier(
          MTL::BarrierScopeBuffers, active_producer_stages, consumer_stages);
      pass_trace::NoteBarrier(current_render_pass_trace_);
      if (render_barrier_gen_ == shared_memory_active_write_gen_) {
        // Same producers as the previous barrier: coverage only grows.
        render_barrier_producers_ |= producers;
        render_barrier_consumers_ |= consumers;
      } else {
        render_barrier_gen_ = shared_memory_active_write_gen_;
        render_barrier_producers_ = producers;
        render_barrier_consumers_ = consumers;
      }
    }
  }
  if (needs_fence_wait && shared_memory_fence_) {
    if (dedup && render_fence_wait_gen_ == shared_memory_fence_update_gen_ &&
        (render_fence_waited_stages_ & consumers) == consumers) {
      ++gpu_time_trace::n_fence_wait_skipped;
    } else {
      pass_trace::NoteFenceWait(current_render_pass_trace_);
      current_render_encoder_->waitForFence(shared_memory_fence_,
                                            consumer_stages);
      if (render_fence_wait_gen_ == shared_memory_fence_update_gen_) {
        render_fence_waited_stages_ |= consumers;
      } else {
        render_fence_wait_gen_ = shared_memory_fence_update_gen_;
        render_fence_waited_stages_ = consumers;
      }
    }
    RetireFenceWaitedSharedMemoryWrites(ranges, range_count);
  }
  return true;
}

bool MetalCommandProcessor::EncodeSharedMemoryBlitReadDependency(
    MTL::BlitCommandEncoder* encoder, uint32_t start, uint32_t length) {
  if (!encoder || !length ||
      !PendingSharedMemoryWritesOverlapRange(start, length)) {
    return true;
  }
  if (!shared_memory_fence_) {
    REXLOG_ERROR(
        "MetalCommandProcessor: blit read cannot wait for shared-memory GPU "
        "writes without a fence");
    return false;
  }
  encoder->waitForFence(shared_memory_fence_);
  SharedMemoryRange range = {start, length};
  RetireFenceWaitedSharedMemoryWrites(&range, 1);
  return true;
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

namespace {
// Trace-only per-frame draw outcome counters (FABLE2_DRAW_SKIP_TRACE=1).
// Default off: no behavior change. Emits "ds_<reason>" rows into the frame CSV
// (duration_us column = count) only for frames with at least one real skip.
struct DrawSkipTrace {
  static bool Enabled() {
    static const bool enabled = [] {
      const char* v = std::getenv("FABLE2_DRAW_SKIP_TRACE");
      return v && v[0] == '1';
    }();
    return enabled;
  }
  static constexpr int kMax = 24;
  const char* names[kMax] = {};
  uint64_t counts[kMax] = {};
  bool benign[kMax] = {};
  int used = 0;
  uint64_t frame = UINT64_MAX;
  void Flush() {
    bool any = false;
    for (int i = 0; i < used; ++i) any |= counts[i] && !benign[i];
    if (any) {
      for (int i = 0; i < used; ++i) {
        if (counts[i]) frame_log::Record(names[i], frame, counts[i]);
      }
    }
    for (int i = 0; i < used; ++i) counts[i] = 0;
  }
  void Add(const char* name, bool is_benign) {
    uint64_t f = frame_log::CurrentFrame();
    if (f != frame) {
      if (frame != UINT64_MAX) Flush();
      frame = f;
    }
    for (int i = 0; i < used; ++i) {
      if (names[i] == name) { ++counts[i]; return; }
    }
    if (used < kMax) {
      names[used] = name; benign[used] = is_benign; counts[used++] = 1;
    }
  }
};
DrawSkipTrace& GetDrawSkipTrace() {
  static DrawSkipTrace trace;
  return trace;
}
}  // namespace

// [alias-clear] The XDK clear (Clear / ClearF of the Xbox 360 Direct3D
// runtime) draws one rectangle with a fixed vertex shader: screen-space
// position (x, y, z, 1) and a colour per vertex from fetch constant 95,
// stride 7 dwords, passed to a pixel shader that writes it to oC0 (or no
// pixel shader for depth/stencil only). To clear 4x faster it draws the
// part that fits into whole tiles with the surface aliased at 4xMSAA (a
// quarter of the pixels), and the rest at the real MSAA level. Only the
// aliased rectangle is taken over here; the rest stays an ordinary draw.
bool MetalCommandProcessor::TryAliasClearDraw(
    xenos::PrimitiveType primitive_type, uint32_t index_count,
    const IndexBufferInfo* index_buffer_info, const Shader& vertex_shader,
    const Shader* pixel_shader, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool memexport_used) {
  constexpr uint64_t kXdkClearVs = UINT64_C(0x1E6883FCCDE1F688);
  constexpr uint64_t kXdkClearPs = UINT64_C(0xA4A965C189287B99);
  if (vertex_shader.ucode_data_hash() != kXdkClearVs ||
      primitive_type != xenos::PrimitiveType::kRectangleList ||
      index_count != 3 || index_buffer_info || memexport_used ||
      (pixel_shader && pixel_shader->ucode_data_hash() != kXdkClearPs)) {
    return false;
  }
  const RegisterFile& regs = *register_file_;
  UploadTraceCounters& u = UploadTrace();
  const char* reason = nullptr;
  auto fail = [&](const char* r, uint32_t& counter) {
    ++counter;
    reason = r;
    return false;
  };
  auto trace_fail = [&]() {
    if (reason && MetalRenderTargetCache::AliasClearTraceEnabled() &&
        MetalRenderTargetCache::ShadowTraceActive()) {
      REXLOG_INFO("[alias-clear] f{} draw kept: {}", frame_log::CurrentFrame(),
                  reason);
    }
  };
  struct TraceOnExit {
    decltype(trace_fail)& f;
    ~TraceOnExit() { f(); }
  } trace_on_exit{trace_fail};

  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const xenos::MsaaSamples msaa = surface_info.msaa_samples;
  const uint32_t pitch_pixels = surface_info.surface_pitch;
  if (msaa == xenos::MsaaSamples::k1X || msaa > xenos::MsaaSamples::k4X ||
      !pitch_pixels) {
    // An ordinary (not aliased) clear, or nothing to alias from.
    return fail("not an MSAA alias", u.ac_fb_state);
  }

  // Fixed-function state: the rectangle covers exactly its pixels and
  // overwrites every sample of them.
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto su = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const auto vtx_cntl = regs.Get<reg::PA_SU_VTX_CNTL>();
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if (vte.vport_x_scale_ena || vte.vport_x_offset_ena ||
      vte.vport_y_scale_ena || vte.vport_y_offset_ena ||
      vte.vport_z_scale_ena || vte.vport_z_offset_ena || !vte.vtx_xy_fmt ||
      !vte.vtx_z_fmt || vte.vtx_w0_fmt || !clip.clip_disable || clip.ucp_ena ||
      su.cull_front || su.cull_back || uint32_t(su.poly_mode) ||
      su.poly_offset_front_enable || su.poly_offset_back_enable ||
      su.poly_offset_para_enable ||
      vtx_cntl.pix_center != xenos::PixelCenter::kD3DZero ||
      // Alpha test and alpha to mask act on the pixel shader's output; with
      // no pixel shader (depth-only clears) they do nothing.
      (pixel_shader && (color_control.alpha_test_enable ||
                        color_control.alpha_to_mask_enable)) ||
      (regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFF) != 0xFFFF ||
      regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET] != 0 ||
      regs[XE_GPU_REG_VGT_INDX_OFFSET] != 0) {
    return fail("rasterizer state", u.ac_fb_state);
  }

  // Vertices.
  const auto& bindings = vertex_shader.vertex_bindings();
  if (bindings.size() != 1 || bindings[0].stride_words != 7) {
    return fail("vertex layout", u.ac_fb_state);
  }
  const xenos::xe_gpu_vertex_fetch_t vf =
      regs.GetVertexFetch(bindings[0].fetch_constant);
  if (vf.type != xenos::FetchConstantType::kVertex || vf.size < 21) {
    return fail("vertex fetch", u.ac_fb_state);
  }
  const auto* vdata = reinterpret_cast<const uint32_t*>(
      memory_->TranslatePhysical(uint32_t(vf.address) << 2));
  if (!vdata) {
    return fail("vertex memory", u.ac_fb_state);
  }
  float v[3][7];
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 7; ++j) {
      uint32_t w = xenos::GpuSwap(vdata[i * 7 + j], vf.endian);
      std::memcpy(&v[i][j], &w, sizeof(float));
    }
  }
  float min_x = std::min({v[0][0], v[1][0], v[2][0]});
  float max_x = std::max({v[0][0], v[1][0], v[2][0]});
  float min_y = std::min({v[0][1], v[1][1], v[2][1]});
  float max_y = std::max({v[0][1], v[1][1], v[2][1]});
  // Three distinct corners of an axis-aligned rectangle (a rectangle list
  // primitive completes the fourth).
  uint32_t corners = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    bool cx0 = v[i][0] == min_x, cx1 = v[i][0] == max_x;
    bool cy0 = v[i][1] == min_y, cy1 = v[i][1] == max_y;
    if ((!cx0 && !cx1) || (!cy0 && !cy1)) {
      return fail("not a rectangle", u.ac_fb_state);
    }
    corners |= 1u << ((cx1 ? 1 : 0) | (cy1 ? 2 : 0));
  }
  if (__builtin_popcount(corners) != 3 || !(min_x < max_x) ||
      !(min_y < max_y)) {
    return fail("not a rectangle", u.ac_fb_state);
  }
  // Pixel centres at integers (D3D9): the rectangle [x - 0.5, ...) covers
  // whole pixels only when its edges sit on half-integers.
  const float fx0 = min_x + 0.5f, fx1 = max_x + 0.5f;
  const float fy0 = min_y + 0.5f, fy1 = max_y + 0.5f;
  if (fx0 < 0.0f || fy0 < 0.0f || fx1 > 8192.0f || fy1 > 8192.0f ||
      fx0 != std::floor(fx0) || fx1 != std::floor(fx1) ||
      fy0 != std::floor(fy0) || fy1 != std::floor(fy1)) {
    return fail("not pixel-aligned", u.ac_fb_rect);
  }
  const uint32_t x0 = uint32_t(fx0), x1 = uint32_t(fx1);
  const uint32_t y0 = uint32_t(fy0), y1 = uint32_t(fy1);
  if (x1 > pitch_pixels) {
    return fail("wider than the pitch", u.ac_fb_rect);
  }
  // Inside the window and screen scissors (no clipping by them).
  const uint32_t ws_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  const uint32_t ws_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  const uint32_t ss_tl = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL];
  const uint32_t ss_br = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR];
  if ((ws_tl & 0x3FFF) > x0 || ((ws_tl >> 16) & 0x3FFF) > y0 ||
      (ws_br & 0x3FFF) < x1 || ((ws_br >> 16) & 0x3FFF) < y1 ||
      (ss_tl & 0x7FFF) > x0 || ((ss_tl >> 16) & 0x7FFF) > y0 ||
      (ss_br & 0x7FFF) < x1 || ((ss_br >> 16) & 0x7FFF) < y1) {
    return fail("scissored", u.ac_fb_rect);
  }
  for (uint32_t j = 2; j < 7; ++j) {
    if (std::memcmp(&v[0][j], &v[1][j], sizeof(float)) ||
        std::memcmp(&v[0][j], &v[2][j], sizeof(float))) {
      return fail("not constant", u.ac_fb_value);
    }
  }

  MetalRenderTargetCache::AliasClearRequest req;
  req.msaa_samples = msaa;
  req.pitch_tiles_at_32bpp =
      ((pitch_pixels << uint32_t(msaa >= xenos::MsaaSamples::k4X)) +
       (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  req.x0 = x0;
  req.y0 = y0;
  req.x1 = x1;
  req.y1 = y1;

  // Depth and stencil: both written with constants (the whole 32-bit word),
  // or neither touched.
  const bool depth_used = normalized_depth_control.z_enable ||
                          normalized_depth_control.stencil_enable;
  // [main-depth] FABLE2_MAIN_DEPTH_NATIVE (docs/main-depth-20261014.md): a
  // stencil-only clear (depth test and write off, stencil ALWAYS/REPLACE,
  // write mask 0xFF) keeps the depth and replaces every stencil byte.
  const bool main_depth_native =
      MetalRenderTargetCache::MainDepthNativeEnabled();
  if (depth_used && main_depth_native && !normalized_depth_control.z_enable &&
      !normalized_color_mask) {
    const auto sref = regs.Get<reg::RB_STENCILREFMASK>();
    const auto sref_bf =
        regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
    if (!normalized_depth_control.stencil_enable ||
        normalized_depth_control.stencilfunc !=
            xenos::CompareFunction::kAlways ||
        normalized_depth_control.stencilzpass != xenos::StencilOp::kReplace ||
        sref.stencilwritemask != 0xFF ||
        (normalized_depth_control.backface_enable &&
         (normalized_depth_control.stencilfunc_bf !=
              xenos::CompareFunction::kAlways ||
          normalized_depth_control.stencilzpass_bf !=
              xenos::StencilOp::kReplace ||
          sref_bf.stencilref != sref.stencilref ||
          sref_bf.stencilwritemask != 0xFF))) {
      return fail("stencil-only state", u.ac_fb_state);
    }
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    req.depth = true;
    req.stencil_only = true;
    req.depth_base = depth_info.depth_base;
    req.depth_format = depth_info.depth_format;
    req.depth_value = sref.stencilref;
  } else if (depth_used) {
    const auto sref = regs.Get<reg::RB_STENCILREFMASK>();
    if (!normalized_depth_control.z_enable ||
        !normalized_depth_control.z_write_enable ||
        normalized_depth_control.zfunc != xenos::CompareFunction::kAlways ||
        !normalized_depth_control.stencil_enable ||
        normalized_depth_control.stencilfunc !=
            xenos::CompareFunction::kAlways ||
        normalized_depth_control.stencilzpass != xenos::StencilOp::kReplace ||
        (normalized_depth_control.backface_enable &&
         (normalized_depth_control.stencilfunc_bf !=
              xenos::CompareFunction::kAlways ||
          normalized_depth_control.stencilzpass_bf !=
              xenos::StencilOp::kReplace ||
          regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilref != sref.stencilref ||
          regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilwritemask != 0xFF)) ||
        sref.stencilwritemask != 0xFF) {
      return fail("depth/stencil state", u.ac_fb_state);
    }
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    const float z = v[0][2];
    uint32_t z24;
    uint32_t z_bits;
    std::memcpy(&z_bits, &z, sizeof(z_bits));
    if (z_bits == 0 || z < 0.0f) {
      // Negative depth is clamped to 0 (clip_disable: depth clamp, as the
      // draw path sets DepthClipModeClamp).
      z24 = 0;
    } else if (z == 1.0f &&
               depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24S8) {
      z24 = 0xFFFFFF;
    } else if (main_depth_native && z > 0.0f && z < 1.0f &&
               depth_info.depth_format ==
                   xenos::DepthRenderTargetFormat::kD24S8) {
      // [main-depth] Any unorm24 grid value (z = 0.75 for the screen-space
      // shadow mask): the transfer shaders read z as
      // RoundToNearestEven(z * 16777215.0f) in float32, and every depth
      // written back from that word is UNorm24To32(word), which must be z
      // itself so later depth readers get the same host value too.
      const float scaled = z * 16777215.0f;
      const uint32_t n24 = uint32_t(std::nearbyint(scaled));
      const float back = xenos::UNorm24To32(n24);
      uint32_t back_bits;
      std::memcpy(&back_bits, &back, sizeof(back_bits));
      if (n24 > 0xFFFFFF || back_bits != z_bits) {
        return fail("depth value", u.ac_fb_value);
      }
      z24 = n24;
    } else {
      return fail("depth value", u.ac_fb_value);
    }
    req.depth = true;
    req.depth_base = depth_info.depth_base;
    req.depth_format = depth_info.depth_format;
    req.depth_value = (z24 << 8) | sref.stencilref;
  }

  // Colour: render target 0 only, all four channels, overwrite blending.
  if (normalized_color_mask) {
    if (normalized_color_mask != 0xF || !pixel_shader) {
      return fail("colour mask", u.ac_fb_state);
    }
    const auto blend = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[0]);
    if (blend.color_srcblend != xenos::BlendFactor::kOne ||
        blend.color_destblend != xenos::BlendFactor::kZero ||
        blend.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend.alpha_destblend != xenos::BlendFactor::kZero ||
        blend.alpha_comb_fcn != xenos::BlendOp::kAdd) {
      return fail("blending", u.ac_fb_state);
    }
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[0]);
    const xenos::ColorRenderTargetFormat format =
        xenos::GetStorageColorFormat(color_info.color_format);
    switch (format) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        break;
      default:
        // Gamma (host storage may differ) and 64bpp.
        return fail("colour format", u.ac_fb_state);
    }
    const float* c = &v[0][3];
    uint32_t value = 0;
    bool zero = true;
    for (uint32_t j = 0; j < 4; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &c[j], sizeof(bits));
      zero = zero && bits == 0;
    }
    if (!zero) {
      // Only unorm formats, values that are exact steps of the format.
      uint32_t steps[4];
      uint32_t shifts[4];
      if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8) {
        for (uint32_t j = 0; j < 4; ++j) {
          steps[j] = 255;
          shifts[j] = 8 * j;
        }
      } else if (format == xenos::ColorRenderTargetFormat::k_2_10_10_10) {
        for (uint32_t j = 0; j < 3; ++j) {
          steps[j] = 1023;
          shifts[j] = 10 * j;
        }
        steps[3] = 3;
        shifts[3] = 30;
      } else {
        return fail("colour value", u.ac_fb_value);
      }
      if (color_info.color_exp_bias) {
        return fail("colour exponent bias", u.ac_fb_value);
      }
      for (uint32_t j = 0; j < 4; ++j) {
        if (!(c[j] >= 0.0f && c[j] <= 1.0f)) {
          return fail("colour value", u.ac_fb_value);
        }
        float s = c[j] * float(steps[j]);
        float r = std::floor(s + 0.5f);
        if (std::fabs(s - r) > 1.0f / 1024.0f) {
          return fail("colour value", u.ac_fb_value);
        }
        value |= uint32_t(r) << shifts[j];
      }
    }
    req.color = true;
    req.color_base = color_info.color_base;
    req.color_format = format;
    req.color_value = value;
    if (req.depth && req.depth_base == req.color_base) {
      return fail("colour and depth at one base", u.ac_fb_state);
    }
  }
  if (!req.depth && !req.color) {
    return fail("writes nothing", u.ac_fb_state);
  }

  // Draws before the clear go to the GPU first, as before a resolve.
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kRenderTargetUpdate)) {
    return false;
  }
  if (!render_target_cache_->TryAliasClear(req)) {
    reason = "render target cache";
    return false;
  }
  return true;
}

void MetalCommandProcessor::OnDrawPacketForCensus(const void* header,
                                                  bool skipped) {
  if (!hle_census::Enabled()) {
    return;
  }
  hle_census::OnDrawPacket(
      uint32_t(static_cast<const uint8_t*>(header) - memory_->physical_membase()),
      skipped);
}

bool MetalCommandProcessor::OnIndirectBufferForCensus(uint32_t ptr,
                                                      uint32_t count) {
  if (hle_census::Collapse() == hle_census::CollapseMode::kSkipSecondTile &&
      (bin_select_ & 0xC) && !(bin_select_ & 0x80000003ull)) {
    ++hle_census::S().collapse_skipped_ibs;
    return true;
  }
  if (!hle_census::Enabled()) {
    return false;
  }
  hle_census::OnIndirectBuffer(ptr);
  if (hle_census::TilingTraceActive(frame_log::CurrentFrame())) {
    REXLOG_INFO("[tiling-trace] f{} ib {:08X} n={} bin={:X}/{:X}",
                frame_log::CurrentFrame(), ptr, count, bin_select_, bin_mask_);
  }
  return false;
}

void MetalCommandProcessor::OnType3PacketForCensus(uint32_t packet,
                                                   const void* header,
                                                   bool skipped) {
  const uint64_t f = frame_log::CurrentFrame();
  if (!hle_census::Enabled() || !hle_census::TilingTraceActive(f)) {
    return;
  }
  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  const auto* d = static_cast<const uint32_t*>(header);
  std::string data;
  // Data dwords follow the header in the same buffer (rings wrap rarely; trace only).
  for (uint32_t i = 1; i <= count && i <= 4; ++i) {
    data += fmt::format(" {:08X}", __builtin_bswap32(d[i]));
  }
  REXLOG_INFO("[tiling-trace] f{} p3 {:08X} op={:02X} n={} pred={} skip={}{}", f,
              uint32_t(static_cast<const uint8_t*>(header) -
                       memory_->physical_membase()),
              opcode, count, packet & 1, skipped ? 1 : 0, data);
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  if (hle_census::Enabled()) {
    census_draw_site_ = hle_census::CurrentSite();
    if (hle_census::TilingTraceActive(frame_log::CurrentFrame())) {
      const RegisterFile& r = *register_file_;
      uint64_t ch = 1469598103934665603ull;  // FNV-1a over float + fetch + bool/loop constants
      for (uint32_t i = XE_GPU_REG_SHADER_CONSTANT_000_X;
           i <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31; ++i) {
        ch = (ch ^ r.values[i]) * 1099511628211ull;
      }
      Shader* vs = active_vertex_shader();
      Shader* ps = active_pixel_shader();
      REXLOG_INFO(
          "[tiling-trace] f{} draw site={} prim={} n={} mode={} wo={:08X} "
          "wsc={:08X}-{:08X} ssc={:08X}-{:08X} surf={:08X} c0={:08X} d={:08X} "
          "vp={}/{} cpy={:08X}/{:08X}/{:08X} bin={:X}/{:X} vs={:016X} ps={:016X} k={:016X}",
          frame_log::CurrentFrame(), census_draw_site_, uint32_t(primitive_type),
          index_count, r[XE_GPU_REG_RB_MODECONTROL] & 7,
          r[XE_GPU_REG_PA_SC_WINDOW_OFFSET], r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
          r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
          r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], r[XE_GPU_REG_RB_SURFACE_INFO],
          r[XE_GPU_REG_RB_COLOR_INFO], r[XE_GPU_REG_RB_DEPTH_INFO],
          r.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
          r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET), r[XE_GPU_REG_RB_COPY_CONTROL],
          r[XE_GPU_REG_RB_COPY_DEST_BASE], r[XE_GPU_REG_RB_COPY_DEST_INFO],
          bin_select_, bin_mask_, vs ? vs->ucode_data_hash() : 0,
          ps ? ps->ucode_data_hash() : 0, ch);
    }
    const xenos::EdramMode mode =
        register_file_->Get<reg::RB_MODECONTROL>().edram_mode;
    hle_census::OnIssueDraw(mode == xenos::EdramMode::kColorDepth ? 0
                            : mode == xenos::EdramMode::kDepthOnly ? 1
                            : mode == xenos::EdramMode::kCopy      ? 2
                                                                   : 3);
  }
  if (zpd_active_report_) {
    // [occlusion-real] count guest draws inside the query, and which of them
    // reached the GPU (a missing draw makes the result "visible").
    uint32_t dispatched_before = 0;
    {
      std::lock_guard<std::mutex> lock(zpd_mutex_);
      auto it = zpd_reports_.find(zpd_active_report_);
      dispatched_before = it != zpd_reports_.end() ? it->second.dispatched : 0;
    }
    draw_skip_reason_ = nullptr;
    bool result = IssueDrawImpl(primitive_type, index_count, index_buffer_info,
                                major_mode_explicit);
    ZpdNoteIssueDraw(true, dispatched_before);
    return result;
  }
  if (!DrawSkipTrace::Enabled()) {
    return IssueDrawImpl(primitive_type, index_count, index_buffer_info,
                         major_mode_explicit);
  }
  draw_skip_reason_ = nullptr;
  bool result = IssueDrawImpl(primitive_type, index_count, index_buffer_info,
                              major_mode_explicit);
  const char* reason = draw_skip_reason_;
  bool is_benign = false;
  if (reason) {
    is_benign = std::strcmp(reason, "ds_submitted") == 0 ||
                std::strcmp(reason, "ds_copy") == 0 ||
                std::strcmp(reason, "ds_pitch0") == 0 ||
                std::strcmp(reason, "ds_rast_off") == 0 ||
                std::strcmp(reason, "ds_no_host_vertices") == 0 ||
                std::strcmp(reason, "ds_alias_noop") == 0 ||
                std::strcmp(reason, "ds_alias_clear") == 0;
  } else {
    reason = result ? "ds_other_true" : "ds_other_false";
  }
  if (!result && is_benign) {
    reason = "ds_submit_failed";
    is_benign = false;
  }
  GetDrawSkipTrace().Add(reason, is_benign);
  if (Shader* vs = active_vertex_shader()) {
    if (vs->is_ucode_analyzed() && vs->memexport_eM_written()) {
      GetDrawSkipTrace().Add("ds_memexport_vs", true);
      if (!is_benign) {
        GetDrawSkipTrace().Add("ds_memexport_vs_skipped", false);
      }
    }
    if (std::strcmp(reason, "ds_geometry_pipeline_failed") == 0 &&
        vs->ucode_data_hash() == UINT64_C(0x695413A9831D88DA)) {
      GetDrawSkipTrace().Add("ds_geomfail_vs695413A9", false);
    }
  }
  return result;
}

bool MetalCommandProcessor::IssueDrawImpl(xenos::PrimitiveType primitive_type,
                                          uint32_t index_count,
                                          IndexBufferInfo* index_buffer_info,
                                          bool major_mode_explicit) {
  ++backend_telemetry_.draw_calls;
  const RegisterFile& regs = *register_file_;
  uint32_t normalized_color_mask = 0;

  // Check for copy mode
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    draw_skip_reason_ = "ds_copy";
    return IssueCopy();
  }

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw.
    draw_skip_reason_ = "ds_pitch0";
    // CR-06 audit counter (FABLE2_DRAW_SKIP_TRACE=1): D3D12/Vulkan skip pitch-0
    // draws only when rasterization is done, so a memexport draw with
    // rasterization off still runs there. Count the ones dropped here.
    if (Shader* vs = DrawSkipTrace::Enabled() ? active_vertex_shader() : nullptr) {
      if (!vs->is_ucode_analyzed()) {
        vs->AnalyzeUcode(pipeline_cache_->ucode_disasm_buffer());
      }
      if (vs->memexport_eM_written()) {
        const bool rast = draw_util::IsRasterizationPotentiallyDone(
            regs, draw_util::IsPrimitivePolygonal(regs));
        GetDrawSkipTrace().Add(rast ? "ds_pitch0_memexport_rast"
                                    : "ds_pitch0_memexport_norast",
                               false);
      }
    }
    return true;
  }

  // Vertex shader analysis.
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    REXLOG_WARN("IssueDraw: No vertex shader");
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(pipeline_cache_->ucode_disasm_buffer());
  }
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;
  // Diagnostic (FABLE2_LOG_MEMEXPORT_FORMATS=1, docs/native-translator.md):
  // log each distinct memexport stream constant (format, signedness) once.
  if (memexport_used_vertex) {
    static const bool log_streams = [] {
      const char* v = std::getenv("FABLE2_LOG_MEMEXPORT_FORMATS");
      return v && v[0] == '1';
    }();
    if (log_streams) {
      static std::mutex log_mutex;
      static std::unordered_set<uint64_t> seen;
      for (uint32_t c : vertex_shader->memexport_stream_constants()) {
        const uint32_t z = regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + 2];
        const uint64_t key = (vertex_shader->ucode_data_hash() << 8) ^ (uint64_t(c) << 40) ^ z;
        std::lock_guard<std::mutex> lock(log_mutex);
        if (seen.insert(key).second) {
          REXLOG_INFO(
              "[memexport] VS {:016X} stream c{}: z {:08X} format {} signed {} "
              "integer {} endian {} red/blue swap {}",
              vertex_shader->ucode_data_hash(), c, z, (z >> 8) & 0x3F,
              (z >> 16) & 1, (z >> 17) & 1, z & 7, (z >> 19) & 1);
        }
      }
    }
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = active_pixel_shader();
      if (pixel_shader) {
        if (!pixel_shader->is_ucode_analyzed()) {
          pixel_shader->AnalyzeUcode(pipeline_cache_->ucode_disasm_buffer());
        }
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      draw_skip_reason_ = "ds_rast_off";
      return true;
    }
  }
  bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  // Primitive/index processing (like D3D12/Vulkan).
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_) {
    REXLOG_ERROR("IssueDraw: primitive processor is not initialized");
    return false;
  }
  if (!primitive_processor_->Process(primitive_processing_result)) {
    REXLOG_ERROR("IssueDraw: primitive processing failed");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    draw_skip_reason_ = "ds_no_host_vertices";
    return true;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kMemExportCompute) {
    primitive_processing_result.host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  }

  struct PendingDrawPassTransferGuard {
    MetalRenderTargetCache* cache = nullptr;
    bool Flush() {
      if (!cache || !cache->HasPendingDrawPassTransfers()) {
        return true;
      }
      // The draw was not encoded: [deferred-clears] clears stay deferred.
      return cache->FlushPendingDrawPassTransfers(/*requeue_clears=*/true);
    }
    ~PendingDrawPassTransferGuard() { (void)Flush(); }
  } pending_draw_pass_transfer_guard;

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  bool use_tessellation_emulation = false;
  if (primitive_processing_result.IsTessellated()) {
    if (!mesh_shader_supported_) {
      static bool tess_mesh_logged = false;
      if (!tess_mesh_logged) {
        tess_mesh_logged = true;
        REXLOG_WARN(
            "Metal: Tessellation emulation requested but mesh shaders are not "
            "supported on this device");
      }
      draw_skip_reason_ = "ds_tess_no_mesh";
      return true;
    }
    if (!pixel_shader) {
      static bool tess_no_ps_logged = false;
      if (!tess_no_ps_logged) {
        tess_no_ps_logged = true;
        REXLOG_WARN(
            "Metal: Tessellation emulation requested without a pixel shader; "
            "using depth-only PS fallback");
      }
    }
    use_tessellation_emulation = true;
  }

  // [main-depth] diagnostic EDRAM dumps at three points of the screen-space
  // shadow-mask sequence in one frame (FABLE2_MAIN_DEPTH_DUMP=<frame>,
  // docs/main-depth-20261014.md): the z = 0.75 alias clear, the shadow-mask
  // draw and the first main colour pass draw, each before its own handling.
  if (render_target_cache_ && MetalRenderTargetCache::MainDepthDumpFrame() &&
      frame_log::CurrentFrame() == MetalRenderTargetCache::MainDepthDumpFrame()) {
    static uint32_t dumped = 0;
    const uint64_t vs_hash = vertex_shader->ucode_data_hash();
    const char* tag = nullptr;
    uint32_t bit = 0;
    if (vs_hash == UINT64_C(0x1E6883FCCDE1F688) &&
        regs[XE_GPU_REG_RB_SURFACE_INFO] == 0x08C20230u &&
        regs[XE_GPU_REG_RB_DEPTH_INFO] == 0) {
      tag = "a-preclear";
      bit = 1;
    } else if (vs_hash == UINT64_C(0x7A6E981FC4D0A375)) {
      tag = "b-ssmask";
      bit = 2;
    } else if (vs_hash == UINT64_C(0xBEAD84BD72072E0E)) {
      tag = "c-mainpass";
      bit = 4;
    }
    if (tag && !(dumped & bit)) {
      dumped |= bit;
      if (FlushPreparedDrawQueue(PreparedDrawFlushReason::kRenderTargetUpdate)) {
        render_target_cache_->MainDepthDumpEdram(tag);
      }
    }
  }

  // [alias-clear] FABLE2_ALIAS_CLEAR_NATIVE (default on,
  // docs/alias-clear-20261013.md).
  if (render_target_cache_ && MetalRenderTargetCache::AliasClearNativeEnabled() &&
      !use_tessellation_emulation) {
    const uint32_t ac_color_mask =
        pixel_shader ? draw_util::GetNormalizedColorMask(
                           regs, pixel_shader->writes_color_targets())
                     : 0;
    const bool ac_depth_used = normalized_depth_control.z_enable ||
                               normalized_depth_control.stencil_enable;
    // A draw that binds, writes and exports nothing has no effect: the
    // render target cache would bind nothing for it either, and occlusion
    // queries do not count host samples (fixed sample count). The XDK emits
    // 24 such point draws after each clear.
    if (!memexport_used_vertex && !memexport_used_pixel && !ac_color_mask &&
        !ac_depth_used) {
      ++UploadTrace().ac_noop;
      draw_skip_reason_ = "ds_alias_noop";
      return true;
    }
    if (TryAliasClearDraw(primitive_type, index_count, index_buffer_info,
                          *vertex_shader, pixel_shader, normalized_depth_control,
                          ac_color_mask, memexport_used_vertex ||
                                             memexport_used_pixel)) {
      draw_skip_reason_ = "ds_alias_clear";
      return true;
    }
  }

  // Configure render targets via MetalRenderTargetCache, similar to D3D12.
  // Update() may internally call PerformTransfersAndResolveClears for EDRAM
  // ownership transfers -- this is the draw-path transfer entry point and
  // no transfer operations bypass the render target cache.
  PreparedDrawRenderTargetKey render_target_key = {};
  if (render_target_cache_) {
    uint32_t ps_writes_color_targets =
        pixel_shader ? pixel_shader->writes_color_targets() : 0;
    normalized_color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(
                                               regs, ps_writes_color_targets)
                                         : 0;
    render_target_key = BuildPreparedDrawRenderTargetKey(
        regs, is_rasterization_done, normalized_depth_control,
        normalized_color_mask);
    if (!prepared_draw_queue_.empty()) {
      if (!prepared_draw_queue_render_target_key_valid_) {
        if (!FlushPreparedDrawQueue(
                PreparedDrawFlushReason::kRenderTargetUpdate)) {
          return false;
        }
      } else if (render_target_key != prepared_draw_queue_render_target_key_) {
        if (!FlushPreparedDrawQueue(
                PreparedDrawFlushReason::kRenderTargetKeyMismatch)) {
          return false;
        }
      }
    }
    // [native-scale] Fable II texture-space draws at 1x
    // (FABLE2_TEXSPACE_NATIVE). The bound target's scale changes with the
    // hint, so queued draws go out first when it flips.
    const bool texspace_hint =
        pixel_shader && texture_cache_ &&
        texture_cache_->IsDrawResolutionScaled() &&
        MetalRenderTargetCache::TexSpaceNativeEnabled() &&
        MetalRenderTargetCache::IsTexSpacePixelShader(
            pixel_shader->ucode_data_hash());
    if (texspace_hint != last_texspace_hint_ && !prepared_draw_queue_.empty() &&
        !FlushPreparedDrawQueue(PreparedDrawFlushReason::kRenderTargetUpdate)) {
      return false;
    }
    last_texspace_hint_ = texspace_hint;
    // [alias-clear] diagnostic (FABLE2_ALIAS_CLEAR_TRACE=1 inside the
    // FABLE2_SHADOW_TRACE window): full state of clear-like draws.
    {
      static const bool ac_trace = [] {
        const char* v = std::getenv("FABLE2_ALIAS_CLEAR_TRACE");
        return v && v[0] == '1';
      }();
      if (ac_trace && MetalRenderTargetCache::ShadowTraceActive()) {
        const auto prim = primitive_type;
        const bool cand =
            vertex_shader->ucode_data_hash() == UINT64_C(0x1E6883FCCDE1F688) ||
            prim == xenos::PrimitiveType::kRectangleList ||
            (prim == xenos::PrimitiveType::kPointList && !pixel_shader);
        if (cand) {
          std::string vtx;
          for (const auto& binding : vertex_shader->vertex_bindings()) {
            xenos::xe_gpu_vertex_fetch_t vf =
                regs.GetVertexFetch(binding.fetch_constant);
            uint32_t addr = vf.address << 2;
            uint32_t len = std::min<uint32_t>(vf.size << 2, 64);
            vtx += fmt::format(" vb{}@{:08X}/s{}:", binding.fetch_constant,
                               addr, binding.stride_words * 4);
            const auto* g = static_cast<const uint8_t*>(
                memory_->TranslatePhysical(addr));
            for (uint32_t o = 0; g && o + 4 <= len; o += 4) {
              uint32_t w = (uint32_t(g[o]) << 24) | (uint32_t(g[o + 1]) << 16) |
                           (uint32_t(g[o + 2]) << 8) | g[o + 3];
              float f;
              std::memcpy(&f, &w, 4);
              vtx += fmt::format(" {:g}", f);
            }
          }
          std::string vc, pc;
          for (uint32_t i = 0; i < 4 * 8; ++i) {
            vc += fmt::format("{}{:g}", i ? "," : "",
                              regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i));
          }
          for (uint32_t i = 0; i < 4 * 4; ++i) {
            pc += fmt::format("{}{:g}", i ? "," : "",
                              regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i));
          }
          REXLOG_INFO(
              "[alias-clear] f{} cand prim={} n={} vs={:016X} ps={:016X} "
              "surf={:08X} ci0={:08X} ci1={:08X} di={:08X} cm={:08X} "
              "zc={:08X} sref={:08X} blend0={:08X} cc={:08X} hiz={:08X} "
              "mode={:08X} vte={:08X} clip={:08X} su={:08X} woff={:08X} "
              "ws={:08X}-{:08X} ss={:08X}-{:08X} vp={:g},{:g},{:g},{:g},{:g},{:g} "
              "aa={:08X} dclr={:08X} cclr={:08X} ps_writes={:X} rast={} "
              "mex={}{} vc=[{}] pc=[{}]",
              frame_log::CurrentFrame(), uint32_t(prim), index_count,
              vertex_shader->ucode_data_hash(),
              pixel_shader ? pixel_shader->ucode_data_hash() : 0,
              regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
              regs[XE_GPU_REG_RB_COLOR1_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
              regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_DEPTHCONTROL],
              regs[XE_GPU_REG_RB_STENCILREFMASK],
              regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
              regs[XE_GPU_REG_RB_HIZCONTROL], regs[XE_GPU_REG_RB_MODECONTROL],
              regs[XE_GPU_REG_PA_CL_VTE_CNTL], regs[XE_GPU_REG_PA_CL_CLIP_CNTL],
              regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
              regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
              regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
              regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
              regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
              regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
              regs[XE_GPU_REG_PA_SC_AA_MASK], regs[XE_GPU_REG_RB_DEPTH_CLEAR],
              regs[XE_GPU_REG_RB_COLOR_CLEAR],
              pixel_shader ? pixel_shader->writes_color_targets() : 0,
              is_rasterization_done ? 1 : 0, memexport_used_vertex ? 1 : 0,
              vtx, vc, pc);
        }
      }
    }
    render_target_cache_->SetTexSpaceDrawHint(texspace_hint);
    const bool rt_updated = render_target_cache_->Update(
        is_rasterization_done, normalized_depth_control, normalized_color_mask,
        *vertex_shader);
    render_target_cache_->SetTexSpaceDrawHint(false);
    if (!rt_updated) {
      REXLOG_ERROR(
          "MetalCommandProcessor::IssueDraw - RenderTargetCache::Update "
          "failed");
      return false;
    }
    pending_draw_pass_transfer_guard.cache = render_target_cache_.get();
    if (current_render_encoder_ &&
        render_target_cache_->IsRenderPassDescriptorDirty() &&
        !render_target_cache_->IsRenderPassDescriptorCompatible(
            current_render_pass_descriptor_, 1)) {
      EndRenderEncoder(
          RenderEncoderEndReason::kRenderTargetUpdateDescriptorDirty);
    }
  }

  // Cast to MSC-specific shader types for the rest of this path.
  auto* metal_vertex_shader = static_cast<MetalShader*>(vertex_shader);
  auto* metal_pixel_shader = static_cast<MetalShader*>(pixel_shader);

  MTL::RenderPipelineState* pipeline = nullptr;
  // Select per-draw shader modifications (mirrors D3D12 PipelineCache).
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (pixel_shader) {
    interpolator_mask = vertex_shader->writes_interpolators() &
                        pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  Shader::HostVertexShaderType host_vertex_shader_type_for_translation =
      primitive_processing_result.host_vertex_shader_type;
  if (host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    if (!mesh_shader_supported_) {
      static bool host_vs_expansion_logged = false;
      if (!host_vs_expansion_logged) {
        host_vs_expansion_logged = true;
        REXLOG_WARN(
            "Metal: Host VS expansion requested without mesh shader support; "
            "skipping draw");
      }
      draw_skip_reason_ = "ds_host_vs_no_mesh";
      return pending_draw_pass_transfer_guard.Flush();
    }
    // Geometry emulation handles point/rectangle expansion; use the normal
    // vertex shader translation path to avoid unsupported host VS types.
    host_vertex_shader_type_for_translation =
        Shader::HostVertexShaderType::kVertex;
  }
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, host_vertex_shader_type_for_translation,
          interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                         *pixel_shader, interpolator_mask, ps_param_gen_pos,
                         normalized_depth_control)
                   : DxbcShaderTranslator::Modification(0);
  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT: the variant with the effects
  // footprint code only when it can apply in this draw.
  if (pixel_shader && FootprintVariantNeeded(*pixel_shader)) {
    pixel_shader_modification.value |=
        MetalPipelineCache::kPixelModificationFootprint;
  }
  // [effects-native] the pixel position of a native-resolution target is not
  // scaled (only shaders that read it get a separate translation).
  if (pixel_shader && render_target_cache_ &&
      render_target_cache_->IsCurrentPassNativeScale() &&
      (pixel_shader_modification.pixel.param_gen_enable ||
       pixel_shader->memexport_eM_written())) {
    pixel_shader_modification.pixel.render_target_native_scale = 1;
  }

  // CR-03 (FABLE2_MEMEXPORT_GEOM_FALLBACK; docs/cr-fixes-20261007.md,
  // docs/weak-mac-perf-20261008.md): a memexport point/rect/quad-list draw
  // without a pixel shader needs geometry emulation, but Metal Shader
  // Converter fails to link (materializeAll) the emulated pipeline that carries
  // user clip distances into the depth-only fallback PS (VS 695413A9, 1 clip
  // plane, D32S8 4x), so those draws were skipped. They compute per-patch
  // tessellation factors (values 0..15) that the tessellated draws of VS
  // 5003700B7C9B1C16 read as their "index" stream; with the skip every patch
  // got a stale 1.0. VS 695413A9 writes oPos = (0,0,0,0) (sgt of -|x| > 0), so
  // it never produces fragments: only its export matters.
  // Default (unset/1): VS 695413A9 runs export-only: a plain vertex pipeline
  // with no fragment function, no geometry emulation, no depth-only PS and no
  // depth-attachment requirement (its points have w = 0 and are clipped).
  // Metal's rasterizationEnabled = NO needs a void vertex function, which Metal
  // Shader Converter output is not. Other matching VS hashes take mode 3.
  // 3 = the previous default: geometry emulation with the clip planes dropped
  // (plain-point fallback if a pipeline still fails). 2 = plain points
  // (rasterized, no geometry emulation). 0 = old skip.
  static const int memexport_geometry_mode = [] {
    const char* v = std::getenv("FABLE2_MEMEXPORT_GEOM_FALLBACK");
    const int mode =
        !v ? 1
           : (v[0] == '0' ? 0 : (v[0] == '2' ? 2 : (v[0] == '3' ? 3 : 1)));
    REXLOG_INFO(
        "[memexport-geom] mode {} (FABLE2_MEMEXPORT_GEOM_FALLBACK={}): {}",
        mode, v ? v : "unset",
        mode == 1 ? "VS 695413A9 export-only (vertex pipeline, no fragment "
                    "stage); other no-PS memexport geometry draws drop user "
                    "clip planes"
        : mode == 3 ? "no-PS memexport geometry draws drop user clip planes; "
                      "plain-point fallback if the pipeline still fails"
        : mode == 2 ? "no-PS memexport geometry draws with clip planes are "
                      "drawn as plain points (no geometry emulation)"
                    : "off: failing geometry pipelines skip their draws");
    return mode;
  }();
  const bool memexport_geometry_fallback = memexport_geometry_mode != 0;
  bool memexport_plain_points = false;
  const bool memexport_no_ps_point_like =
      !pixel_shader && memexport_used_vertex &&
      !primitive_processing_result.IsTessellated() &&
      (primitive_processing_result.host_primitive_type ==
           xenos::PrimitiveType::kPointList ||
       primitive_processing_result.host_primitive_type ==
           xenos::PrimitiveType::kRectangleList ||
       primitive_processing_result.host_primitive_type ==
           xenos::PrimitiveType::kQuadList);
  // Mode 1 covers every VS 695413A9 draw, with or without clip planes (about
  // half of them have none and used to go through geometry emulation too).
  const bool memexport_export_only =
      memexport_geometry_mode == 1 && memexport_no_ps_point_like &&
      vertex_shader->ucode_data_hash() == UINT64_C(0x695413A9831D88DA);
  if (memexport_export_only ||
      (memexport_geometry_fallback && memexport_no_ps_point_like &&
       vertex_shader_modification.vertex.user_clip_plane_count)) {
    static std::atomic<uint64_t> handled{0};
    const uint64_t n = handled.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 100000 == 0) {
      REXLOG_INFO(
          "[memexport-geom] VS {:016X}: no-PS memexport geometry draw with {} "
          "user clip plane(s): {} (#{})",
          vertex_shader->ucode_data_hash(),
          uint32_t(vertex_shader_modification.vertex.user_clip_plane_count),
          memexport_export_only          ? "export-only"
          : memexport_geometry_mode == 2 ? "plain points"
                                         : "clip planes dropped",
          n);
    }
    if (memexport_geometry_mode == 2 || memexport_export_only) {
      memexport_plain_points = true;
    }
    if (memexport_geometry_mode != 2) {
      vertex_shader_modification.vertex.user_clip_plane_count = 0;
      vertex_shader_modification.vertex.user_clip_plane_cull = 0;
    }
  }

  PipelineGeometryShader geometry_shader_type = PipelineGeometryShader::kNone;
  if (!primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_shader_type = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        geometry_shader_type = PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_shader_type = PipelineGeometryShader::kQuadList;
        break;
      default:
        break;
    }
  }

  GeometryShaderKey geometry_shader_key;
  bool use_geometry_emulation = false;
  if (memexport_plain_points) {
    geometry_shader_type = PipelineGeometryShader::kNone;
  }
  if (geometry_shader_type != PipelineGeometryShader::kNone) {
    bool can_build_geometry_shader =
        pixel_shader || !vertex_shader_modification.vertex.interpolator_mask;
    if (!can_build_geometry_shader) {
      static bool geom_interp_mismatch_logged = false;
      if (!geom_interp_mismatch_logged) {
        geom_interp_mismatch_logged = true;
        REXLOG_WARN(
            "Metal: geometry emulation skipped because pixel shader is null "
            "but vertex interpolators are present");
      }
    } else {
      use_geometry_emulation =
          GetGeometryShaderKey(geometry_shader_type, vertex_shader_modification,
                               pixel_shader_modification, geometry_shader_key);
    }
  }
  if (use_geometry_emulation && !mesh_shader_supported_) {
    static bool mesh_support_logged = false;
    if (!mesh_support_logged) {
      mesh_support_logged = true;
      REXLOG_WARN(
          "Metal: geometry emulation requested but mesh shaders are not "
          "supported on this device");
    }
    use_geometry_emulation = false;
  }
  if (use_geometry_emulation && !pixel_shader) {
    static bool geom_no_ps_logged = false;
    if (!geom_no_ps_logged) {
      geom_no_ps_logged = true;
      REXLOG_WARN(
          "Metal: geometry emulation requested without a pixel shader; using "
          "depth-only PS fallback");
    }
  }

  // Get or create shader translations for the selected modifications.
  auto vertex_translation = static_cast<MetalShader::MetalTranslation*>(
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));

  MetalShader::MetalTranslation* pixel_translation = nullptr;
  if (pixel_shader) {
    pixel_translation = static_cast<MetalShader::MetalTranslation*>(
        pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value));
  }

  if (use_tessellation_emulation || use_geometry_emulation) {
    if (!pipeline_cache_->EnsureDxbcTranslationReady(vertex_translation,
                                                     "vertex")) {
      return false;
    }
    if (!pipeline_cache_->EnsureMetalTranslationReady(pixel_translation)) {
      return false;
    }
  }

  bool use_fallback_ps =
      (use_tessellation_emulation || use_geometry_emulation) &&
      !pixel_translation;

  // Resolve attachment formats once for all pipeline paths.
  bool pixel_shader_writes_depth_for_fmts =
      pixel_translation && pixel_translation->shader().writes_depth();
  if (use_fallback_ps) {
    pixel_shader_writes_depth_for_fmts = true;  // depth-only PS fallback
  }
  bool fallback_depth_attachment_required = pixel_shader_writes_depth_for_fmts;
  if (use_fallback_ps && render_target_cache_ &&
      !render_target_cache_->GetDepthTargetForDraw() &&
      !memexport_used_vertex) {
    static bool fallback_without_depth_logged = false;
    if (!fallback_without_depth_logged) {
      fallback_without_depth_logged = true;
      REXLOG_WARN(
          "Metal: depth-only fallback PS requested without a depth attachment "
          "or memexport side effects; skipping no-output emulated draw");
    }
    draw_skip_reason_ = "ds_depthonly_no_output";
    return pending_draw_pass_transfer_guard.Flush();
  }
  if (render_target_cache_) {
    render_target_key = BuildPreparedDrawRenderTargetKey(
        regs, is_rasterization_done, normalized_depth_control,
        normalized_color_mask);
  }
  if (current_render_encoder_ && render_target_cache_ &&
      !render_target_cache_->IsRenderPassDescriptorCompatible(
          current_render_pass_descriptor_, 1,
          fallback_depth_attachment_required)) {
    EndRenderEncoder(RenderEncoderEndReason::kPipelineDescriptorIncompatible);
  }
  MTL::RenderPassDescriptor* pass_desc_for_fmts =
      current_render_pass_descriptor_;
  if (render_target_cache_) {
    if (MTL::RenderPassDescriptor* cache_desc =
            render_target_cache_->GetRenderPassDescriptor(
                1, fallback_depth_attachment_required)) {
      pass_desc_for_fmts = cache_desc;
    }
  }
  auto attachment_formats = ResolvePipelineAttachmentFormats(
      render_target_cache_.get(), pass_desc_for_fmts,
      pixel_shader_writes_depth_for_fmts, "Pipeline");

  // Derive the shared rendering key (color mask, blend, alpha-to-mask) once
  // for all pipeline paths instead of re-reading registers in each method.
  auto rendering_key =
      ResolvePipelineRenderingKey(regs, pixel_translation, use_fallback_ps);

  MetalPipelineCache::TessellationPipelineState* tessellation_pipeline_state =
      nullptr;
  MetalPipelineCache::GeometryPipelineState* geometry_pipeline_state = nullptr;
  // FABLE2_NS_SPECIALIZE: the standard pipeline handle of this draw (null for
  // emulation pipelines), for the specialized-pipeline lookup below.
  MetalPipelineCache::PipelineHandle* spec_pipeline_handle = nullptr;
  // Diagnostic: FABLE2_SKIP_VS=<hash hex> skips every draw with that vertex
  // shader (what disappears shows what it draws).
  {
    static const uint64_t skip_vs = [] {
      const char* v = std::getenv("FABLE2_SKIP_VS");
      return v ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    if (skip_vs && vertex_shader->ucode_data_hash() == skip_vs) {
      draw_skip_reason_ = "ds_skip_vs";
      return pending_draw_pass_transfer_guard.Flush();
    }
  }
  // FABLE2_EMU_DRAW_STATS (census) and FABLE2_NATIVE_TESS (default 1, exact):
  // docs/native-translator.md, "Geometry and tessellation emulation".
  bool native_tess = false;
  bool native_tess_cpu = false;  // reference tessellator pattern runs
  MTL::Buffer* native_tess_merged_buffer = nullptr;
  uint32_t native_tess_merged_offset = 0;
  uint32_t native_tess_merged_count = 0;
  MTL::Buffer* native_tess_factor_buffer = nullptr;
  uint32_t native_tess_factor_offset = 0;
  uint32_t native_tess_patches = 0;
  bool native_points = false;  // points or rectangles: native_geometry_vertices per instance
  uint32_t native_geometry_vertices = 1;
  bool native_points_mesh = false;  // FABLE2_NATIVE_POINTS=2: mesh function
  if ((use_tessellation_emulation || use_geometry_emulation) &&
      (emu_stats::Enabled() || NativeTessMode() || NativePointsMode() ||
       NativeRectsMode())) {
    const uint64_t vs_hash = vertex_shader->ucode_data_hash();
    if (use_geometry_emulation) {
      // FABLE2_NATIVE_POINTS: point sprites expanded by the native VS.
      const bool expand_points =
          NativePointsMode() &&
          geometry_shader_key.type == PipelineGeometryShader::kPointList;
      const bool expand_rects =
          NativeRectsMode() &&
          geometry_shader_key.type == PipelineGeometryShader::kRectangleList;
      // Diagnostic: FABLE2_NATIVE_POINTS_ONLY=<VS hash hex> limits the native
      // point/rectangle expansion to that vertex shader.
      static const uint64_t native_points_only = [] {
        const char* v = std::getenv("FABLE2_NATIVE_POINTS_ONLY");
        if (v && v[0] && NoConverterForcesNative()) {
          REXLOG_WARN("[no-converter] FABLE2_NATIVE_POINTS_ONLY={} ignored: "
                      "no converter fallback in this build",
                      v);
          return 0ull;
        }
        return v ? std::strtoull(v, nullptr, 16) : 0ull;
      }();
      // Memory-exporting point lists: only the mesh expansion (the VS runs
      // once per point), FABLE2_NATIVE_POINTS_MEMEXPORT.
      if ((expand_points || expand_rects) &&
          (!memexport_used_vertex ||
           (expand_points && NativePointsMemexportMode())) &&
          (!native_points_only || native_points_only == vs_hash)) {
        if (MTL::RenderPipelineState* points_pipeline =
                pipeline_cache_->GetOrCreateNativePointsPipeline(
                    vertex_translation, pixel_translation, geometry_shader_key,
                    attachment_formats, rendering_key)) {
          static uint64_t native_points_draws = 0;
          ++native_points_draws;
          if ((native_points_draws & (native_points_draws - 1)) == 0) {
            REXLOG_INFO("[native-points] {} draws expanded natively",
                        native_points_draws);
          }
          native_points = true;
          native_geometry_vertices = expand_rects ? 3u : 1u;
          native_points_mesh = expand_points && NativePointsMode() == 2;
          use_geometry_emulation = false;
          pipeline = points_pipeline;
        }
      }
      if (emu_stats::Enabled()) {
        uint32_t kind = emu_stats::kGeomPoint;
        if (geometry_shader_key.type == PipelineGeometryShader::kRectangleList) {
          kind = emu_stats::kGeomRect;
        } else if (geometry_shader_key.type == PipelineGeometryShader::kQuadList) {
          kind = emu_stats::kGeomQuad;
        }
        emu_stats::Stats::Get().NoteGeometry(
            frame_current_, kind, vs_hash,
            primitive_processing_result.guest_draw_vertex_count);
      }
    } else {
      float factor_min, factor_max;
      GetTessellationFactorRange(regs, factor_min, factor_max);
      const xenos::TessellationMode mode =
          primitive_processing_result.tessellation_mode;
      bool have_factors =
          mode == xenos::TessellationMode::kAdaptive &&
          ReadAdaptiveTessFactors(primitive_processing_result, factor_min,
                                  factor_max, tess_cp_scratch_);
      // Continuous quads (continuous_quad_1cp_hs): every factor is the
      // maximum; one patch per control point.
      if (mode == xenos::TessellationMode::kContinuous &&
          primitive_processing_result.host_vertex_shader_type ==
              Shader::HostVertexShaderType::kQuadDomainPatchIndexed &&
          NativeTessMode()) {
        tess_cp_scratch_.assign(
            size_t(primitive_processing_result.host_draw_vertex_count) * 4,
            factor_max);
        have_factors = !tess_cp_scratch_.empty();
      }
      if (emu_stats::Enabled()) {
        const uint32_t kind =
            mode == xenos::TessellationMode::kAdaptive
                ? emu_stats::kTessAdaptive
                : (mode == xenos::TessellationMode::kContinuous
                       ? emu_stats::kTessContinuous
                       : emu_stats::kTessDiscrete);
        // Uniform modes: one patch per control point (1-CP patches) at the
        // maximum factor; adaptive: 4 factors per patch.
        const bool adaptive_factors =
            have_factors && mode == xenos::TessellationMode::kAdaptive;
        const uint32_t patches =
            adaptive_factors
                ? uint32_t(tess_cp_scratch_.size() / 4)
                : primitive_processing_result.host_draw_vertex_count;
        emu_stats::Stats::Get().NoteTessellation(
            frame_current_, kind, vs_hash, patches,
            adaptive_factors ? tess_cp_scratch_.data() : nullptr, factor_max);
      }
      // Blending on any written render target: the tessellator emits a
      // draw's triangles in another order than the emulation, which only
      // matters where they blend over each other (or tie in depth).
      bool tess_blended = false;
      for (uint32_t rt = 0; rt < 4; ++rt) {
        if (((rendering_key.normalized_color_mask >> (rt * 4)) & 0xF) &&
            (rendering_key.blendcontrol[rt] & 0x1FFF1FFFu) != 0x00010001u) {
          tess_blended = true;
        }
      }
      if (emu_stats::Enabled()) {
        // Once per state combination: the blend / depth state of every
        // tessellated draw (docs/native-translator.md, primitive order).
        static std::unordered_set<uint64_t> logged_states;
        const uint64_t state_key[] = {
            vs_hash, pixel_shader ? pixel_shader->ucode_data_hash() : 0,
            uint64_t(rendering_key.blendcontrol[0]) |
                (uint64_t(rendering_key.normalized_color_mask) << 32),
            uint64_t(normalized_depth_control.value)};
        if (logged_states.insert(XXH3_64bits(state_key, sizeof(state_key)))
                .second) {
          REXLOG_INFO(
              "[emu-stats] tess state: VS {:016X} PS {:016X} mode {} blend0 "
              "{:08X} color mask {:04X} blended {} z_enable {} z_write {} "
              "zfunc {}",
              vs_hash, state_key[1], uint32_t(mode),
              rendering_key.blendcontrol[0], rendering_key.normalized_color_mask,
              tess_blended ? 1 : 0, uint32_t(normalized_depth_control.z_enable),
              uint32_t(normalized_depth_control.z_write_enable),
              uint32_t(normalized_depth_control.zfunc));
        }
      }
      // FABLE2_NATIVE_TESS: which path a draw takes.
      // 1 (exact): patches all uniform and half-exact -> Metal's tessellator
      //   (bit-identical to the emulation); otherwise the D3D11 reference
      //   tessellator's pattern (exact D3D semantics, correct edges).
      // 2: Metal's tessellator for everything. 3: the reference for
      //   everything. Blended draws: Metal's tessellator when their factors
      //   are uniform and half-exact and FABLE2_NATIVE_TESS_BLENDED is on
      //   (default; triangle order inside a draw accepted as is, 2026-10-14),
      //   all of them in mode 2, otherwise emulated.
      const uint32_t tess_mode = NativeTessMode();
      const bool blended_native = tess_blended && NativeTessBlendedMode();
      bool tess_exact = !(tess_blended && tess_mode != 2 && !blended_native);
      bool tess_cpu = tess_exact && tess_mode == 3 && !tess_blended;
      if (tess_exact && have_factors &&
          (tess_mode == 1 || (tess_mode == 3 && tess_blended))) {
        const float* cp = tess_cp_scratch_.data();
        bool uniform = true;
        for (size_t i = 0; i + 3 < tess_cp_scratch_.size() && uniform;
             i += 4) {
          uniform = cp[i] == cp[i + 1] && cp[i] == cp[i + 2] &&
                    cp[i] == cp[i + 3] && float(__fp16(cp[i])) == cp[i];
        }
        if (tess_blended) {
          // Only the tessellator order was accepted for blended draws.
          // No-converter build: there is no emulation to keep non-uniform
          // blended draws on (FABLE2_TERRAIN_ADAPTIVE=1), so they take
          // Metal's tessellator as in mode 2 instead of being refused.
          tess_exact = uniform || NoConverterForcesNative();
          if (!uniform && NoConverterForcesNative()) {
            static bool logged = false;
            if (!logged) {
              logged = true;
              REXLOG_WARN("[no-converter] non-uniform blended tessellated "
                          "draws take Metal's tessellator (no emulation "
                          "fallback in this build)");
            }
          }
          tess_cpu = false;
        } else {
          tess_cpu = !uniform;
        }
      }
      // The reference pattern path (exact): runs of consecutive patches
      // sharing a pattern are drawn instanced.
      if (tess_cpu && have_factors && !memexport_used_vertex) {
        native_tess_runs_scratch_.clear();
        bool runs_ok = true;
        const float* cp = tess_cp_scratch_.data();
        const uint32_t patches = uint32_t(tess_cp_scratch_.size() / 4);
        for (uint32_t p = 0; p < patches && runs_ok; ++p, cp += 4) {
          // The adaptive hull shader: edge i = control point (i + 3) & 3,
          // inside U = min(cp2, cp0), inside V = min(cp1, cp3).
          const float f[6] = {cp[3], cp[0], cp[1], cp[2], std::fmin(cp[2], cp[0]),
                              std::fmin(cp[1], cp[3])};
          const TessPattern* pattern = GetTessPattern(f);
          if (!pattern) {
            runs_ok = false;
            break;
          }
          if (!native_tess_runs_scratch_.empty() &&
              native_tess_runs_scratch_.back().pattern == pattern) {
            ++native_tess_runs_scratch_.back().count;
          } else {
            native_tess_runs_scratch_.push_back({pattern, p, 1});
          }
        }
        MTL::RenderPipelineState* cpu_pipeline =
            runs_ok ? pipeline_cache_->GetOrCreateNativeTessPipeline(
                          vertex_translation, pixel_translation,
                          primitive_processing_result, attachment_formats,
                          rendering_key, true)
                    : nullptr;
        if (cpu_pipeline) {
          static uint64_t cpu_tess_draws = 0;
          ++cpu_tess_draws;
          if ((cpu_tess_draws & (cpu_tess_draws - 1)) == 0) {
            REXLOG_INFO("[native-tess] {} draws with the reference tessellator "
                        "({} patterns cached)",
                        cpu_tess_draws, tess_patterns_.size());
          }
          // One indexed draw for the whole guest draw when it fits: index =
          // (patch << 20) | arena point, uploaded per draw.
          native_tess_merged_buffer = nullptr;
          size_t merged_count = 0;
          for (const TessRun& run : native_tess_runs_scratch_) {
            merged_count += size_t(run.pattern->index_count) * run.count;
          }
          if (patches < 4095 && merged_count &&
              merged_count * sizeof(uint32_t) <= (size_t(2) << 20)) {
            size_t offset = 0;
            uint64_t gpu_address = 0;
            MTL::Buffer* buffer = nullptr;
            auto* out = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
                frame_current_, merged_count * sizeof(uint32_t), 16, &buffer,
                offset, gpu_address));
            if (out) {
              for (const TessRun& run : native_tess_runs_scratch_) {
                const uint32_t* local = run.pattern->local_indices.data();
                const uint32_t n = run.pattern->index_count;
                const uint32_t base = run.pattern->arena_base;
                for (uint32_t p = run.first; p < run.first + run.count; ++p) {
                  const uint32_t add = (p << 20) | base;
                  for (uint32_t i = 0; i < n; ++i) {
                    *out++ = add + local[i];
                  }
                }
              }
              native_tess_merged_buffer = buffer;
              native_tess_merged_offset = uint32_t(offset);
              native_tess_merged_count = uint32_t(merged_count);
            }
          }
          native_tess = true;
          native_tess_cpu = true;
          native_tess_patches = patches;
          use_tessellation_emulation = false;
          pipeline = cpu_pipeline;
          tess_exact = false;  // skip the tessellator path below
        } else if (tess_mode == 3) {
          tess_exact = false;  // emulation
        }
      }
      if (!tess_exact && have_factors && !native_tess) {
        static uint64_t inexact_draws = 0;
        ++inexact_draws;
        if ((inexact_draws & (inexact_draws - 1)) == 0) {
          REXLOG_INFO("[native-tess] {} draws kept on emulation (blended: "
                      "FABLE2_NATIVE_TESS_BLENDED off or non-uniform factors)",
                      inexact_draws);
        }
      }
      if (NativeTessMode() && have_factors && tess_exact && !native_tess &&
          !memexport_used_vertex) {
        MTL::RenderPipelineState* native_pipeline =
            pipeline_cache_->GetOrCreateNativeTessPipeline(
                vertex_translation, pixel_translation,
                primitive_processing_result, attachment_formats,
                rendering_key);
        const uint32_t patches = uint32_t(tess_cp_scratch_.size() / 4);
        // MTLQuadTessellationFactorsHalf: 4 edge + 2 inside halves.
        constexpr size_t kPatchBytes = 12;
        MTL::Buffer* buffer = nullptr;
        size_t offset = 0;
        uint64_t gpu_address = 0;
        uint8_t* data =
            native_pipeline
                ? constant_buffer_pool_->Request(frame_current_,
                                                 patches * kPatchBytes, 16,
                                                 &buffer, offset, gpu_address)
                : nullptr;
        if (data) {
          // The adaptive hull shader: edge i = control point (i + 3) & 3,
          // inside U = min(cp2, cp0), inside V = min(cp1, cp3). Experiment
          // knob FABLE2_NATIVE_TESS_EDGE_MAP=<4 digits> (control point per
          // Metal edge) for the parity harness.
          static const std::array<uint32_t, 4> edge_map = [] {
            std::array<uint32_t, 4> m = {3, 0, 1, 2};
            const char* v = std::getenv("FABLE2_NATIVE_TESS_EDGE_MAP");
            if (v && std::strlen(v) == 4) {
              for (uint32_t i = 0; i < 4; ++i) {
                m[i] = uint32_t(v[i] - '0') & 3;
              }
            }
            return m;
          }();
          auto* out = reinterpret_cast<__fp16*>(data);
          const float* cp = tess_cp_scratch_.data();
          for (uint32_t p = 0; p < patches; ++p, cp += 4, out += 6) {
            for (uint32_t i = 0; i < 4; ++i) {
              out[i] = __fp16(cp[edge_map[i]]);
            }
            out[4] = __fp16(std::fmin(cp[2], cp[0]));
            out[5] = __fp16(std::fmin(cp[1], cp[3]));
          }
          static uint64_t native_tess_draws = 0;
          ++native_tess_draws;
          if ((native_tess_draws & (native_tess_draws - 1)) == 0) {
            REXLOG_INFO("[native-tess] {} draws on the hardware tessellator",
                        native_tess_draws);
          }
          if (tess_blended) {
            static uint64_t blended_draws = 0;
            ++blended_draws;
            if ((blended_draws & (blended_draws - 1)) == 0) {
              REXLOG_INFO("[native-tess] {} blended draws on the hardware "
                          "tessellator",
                          blended_draws);
            }
          }
          native_tess = true;
          native_tess_factor_buffer = buffer;
          native_tess_factor_offset = uint32_t(offset);
          native_tess_patches = patches;
          use_tessellation_emulation = false;
          pipeline = native_pipeline;
        }
      }
      // FABLE2_NATIVE_TESS_TRACE=1 (diagnostic, docs/native-translator.md):
      // which path every tessellated draw took and why, once per combination,
      // plus counts every 600 frames.
      if (NativeTessTraceEnabled()) {
        const char* path;
        if (native_tess) {
          path = native_tess_cpu ? "reference" : "tessellator";
        } else if (!NativeTessMode()) {
          path = "emulation:off";
        } else if (!have_factors) {
          path = "emulation:no-factors";
        } else if (memexport_used_vertex) {
          path = "emulation:memexport";
        } else if (!tess_exact) {
          path = tess_blended ? "emulation:blended" : "emulation:inexact";
        } else {
          path = "emulation:pipeline-or-upload-failed";
        }
        float fmin_seen = 0.0f, fmax_seen = 0.0f;
        bool bad_factor = false;
        if (have_factors && !tess_cp_scratch_.empty()) {
          fmin_seen = fmax_seen = tess_cp_scratch_[0];
          for (float f : tess_cp_scratch_) {
            fmin_seen = std::fmin(fmin_seen, f);
            fmax_seen = std::fmax(fmax_seen, f);
            bad_factor = bad_factor || !(f >= 1.0f && f <= 64.0f);
          }
        }
        NativeTessTrace(vs_hash,
                        pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                        path, uint32_t(mode), tess_blended,
                        uint32_t(tess_cp_scratch_.size() / 4), fmin_seen,
                        fmax_seen, bad_factor,
                        uint32_t(normalized_depth_control.zfunc),
                        uint32_t(normalized_depth_control.z_write_enable));
      }
    }
  }
  if (native_tess || native_points) {
    // The pipeline is set above; neither emulation nor the standard path.
  } else if (use_tessellation_emulation) {
    tessellation_pipeline_state =
        pipeline_cache_->GetOrCreateTessellationPipelineState(
            vertex_translation, pixel_translation, primitive_processing_result,
            attachment_formats, rendering_key);
    // current_pipeline(): the optimized rebuild once FABLE2_NS_EMU_PIPELINES
    // has published it.
    pipeline = tessellation_pipeline_state
                   ? tessellation_pipeline_state->current_pipeline()
                   : nullptr;
    // A refused or failed emulation pipeline loses only this draw. Before
    // 2026-10-14 the draw went on to DispatchDraw with a null state; its
    // null dereference re-faulted forever in the exception handler, so the
    // GPU Commands thread never finished the frame (no-converter build with
    // FABLE2_NATIVE_TESS=0: audio and input on, the screen black).
    if (!tessellation_pipeline_state || !pipeline) {
      static bool tessellation_pipeline_failure_logged = false;
      if (!tessellation_pipeline_failure_logged) {
        tessellation_pipeline_failure_logged = true;
        REXLOG_WARN(
            "Metal: tessellation emulation pipeline unavailable; skipping "
            "those draws");
      }
      draw_skip_reason_ = "ds_tessellation_pipeline_failed";
      return pending_draw_pass_transfer_guard.Flush();
    }
  } else if (use_geometry_emulation) {
    geometry_pipeline_state = pipeline_cache_->GetOrCreateGeometryPipelineState(
        vertex_translation, pixel_translation, geometry_shader_key,
        attachment_formats, rendering_key);
    // FABLE2_MEMEXPORT_GEOM_FALLBACK (above): keep the memexport side
    // effects of a vertex shader whose geometry-emulation pipeline failed by
    // drawing it as plain points instead of skipping it.
    if ((!geometry_pipeline_state || !geometry_pipeline_state->pipeline) &&
        memexport_geometry_fallback && memexport_used_vertex) {
      geometry_pipeline_state = nullptr;
      use_geometry_emulation = false;
      draw_skip_reason_ = "ds_memexport_geom_fallback";
    } else if (!geometry_pipeline_state || !geometry_pipeline_state->pipeline) {
      static bool geometry_pipeline_failure_logged = false;
      if (!geometry_pipeline_failure_logged) {
        geometry_pipeline_failure_logged = true;
        REXLOG_WARN(
            "Metal: geometry emulation pipeline creation failed; skipping "
            "geometry-emulated draws instead of aborting the backend draw "
            "packet");
      }
      draw_skip_reason_ = "ds_geometry_pipeline_failed";
      return pending_draw_pass_transfer_guard.Flush();
    }
    if (geometry_pipeline_state) {
      pipeline = geometry_pipeline_state->current_pipeline();
    }
  }
  if (!use_tessellation_emulation && !use_geometry_emulation && !native_tess &&
      !native_points) {
    // Trace-only (docs/native-shaders.md): FABLE2_NS_LOG_FETCH=<PS hash hex>
    // logs the fetch constants used by the first 16 draws of that pixel
    // shader, for specialization experiments.
    {
      static const uint64_t log_ps = [] {
        const char* v = std::getenv("FABLE2_NS_LOG_FETCH");
        return v ? std::strtoull(v, nullptr, 16) : 0ull;
      }();
      static uint32_t logged = 0;
      if (log_ps && pixel_shader && logged < 16 &&
          pixel_shader->ucode_data_hash() == log_ps) {
        ++logged;
        std::string line;
        for (const auto& binding : pixel_shader->texture_bindings()) {
          const uint32_t* f =
              &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                           binding.fetch_constant * 6];
          line += fmt::format(" tf{}:{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
                              binding.fetch_constant, f[0], f[1], f[2], f[3],
                              f[4], f[5]);
        }
        REXLOG_INFO("[ns-fetch] ps {:016X} mod {:X} draw {}:{}",
                    pixel_shader->ucode_data_hash(),
                    pixel_shader_modification.value, logged, line);
      }
    }
    auto* pipeline_handle = pipeline_cache_->GetOrCreatePipelineState(
        vertex_translation, pixel_translation, attachment_formats,
        rendering_key);
    spec_pipeline_handle = pipeline_handle;
    if (pipeline_handle) {
      pipeline = pipeline_handle->state.load(std::memory_order_acquire);
      // This game-specific pixel shader generates the morph texture immediately
      // before the 0x12704000 readbacks. Skipping its first draws leaves red
      // high-resolution mips permanently baked into the CPU-compressed texture.
      // Do not impose this wait on ordinary scene shaders: that causes stutters
      // whenever gameplay encounters an uncached pipeline.
      const bool preserve_morph_draw =
          REXCVAR_GET(metal_fable_morph_readback) && pixel_shader &&
          pixel_shader->ucode_data_hash() == UINT64_C(0xE2120FEAFA4CF565);
      if (!pipeline && (preserve_morph_draw || pipeline_handle->startup_warmup)) {
        // Cached startup draws must not disappear while the logos play or
        // when the player skips directly into gameplay.
        const uint64_t wait_start = frame_log::Enabled() ? frame_log::NowUs() : 0;
        pipeline = pipeline_cache_->WaitForPipeline(pipeline_handle);
        if (frame_log::Enabled()) frame_log::Record(
            "pipeline_wait", frame_log::CurrentFrame(), frame_log::NowUs() - wait_start);
      }
    }
    if (!pipeline) {
      if (REXCVAR_GET(async_shader_compilation) && pipeline_handle) {
        if (!pipeline_handle->creation_complete.load(std::memory_order_acquire)) {
          draw_skip_reason_ = "ds_async_compiling";
          return pending_draw_pass_transfer_guard.Flush();
        }
        // Completion may have been published after the first state load.
        pipeline = pipeline_handle->state.load(std::memory_order_acquire);
      }
      if (!pipeline) {
        REXLOG_ERROR("Failed to create pipeline state");
        return false;
      }
    }
  }

  {
    // [converter-use] census (metal_converter_census.h): which kind of
    // pipeline this draw runs, and whether any stage is converter-built.
    namespace cc = converter_census;
    const bool ps_native =
        !pixel_translation || (pixel_translation->is_native_xlate() &&
                               pixel_translation->optimized_function());
    if (native_points) {
      cc::CountDraw(ps_native ? cc::kDrawNativeGeom : cc::kDrawNativeGeomConvPS);
    } else if (native_tess) {
      cc::CountDraw(ps_native ? cc::kDrawNativeTess : cc::kDrawNativeTessConvPS);
    } else if (use_tessellation_emulation) {
      cc::CountDraw(cc::kDrawTessEmu);
      static std::unordered_set<uint64_t> logged_tess;
      const uint64_t k[] = {vertex_shader->ucode_data_hash(),
                            pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                            uint64_t(primitive_processing_result.tessellation_mode)};
      if (logged_tess.insert(XXH3_64bits(k, sizeof(k))).second &&
          logged_tess.size() <= 64) {
        REXLOG_INFO(
            "[converter-use] tessellation emulation: VS {:016X} PS {:016X} "
            "mode {} host vs type {} memexport {}",
            k[0], k[1], uint32_t(primitive_processing_result.tessellation_mode),
            uint32_t(primitive_processing_result.host_vertex_shader_type),
            memexport_used_vertex ? 1 : 0);
      }
    } else if (use_geometry_emulation) {
      cc::CountDraw(cc::kDrawGeomEmu);
      // Once per (VS, PS, geometry key): why this draw is emulated.
      static std::unordered_set<uint64_t> logged_geom;
      const uint64_t k[] = {vertex_shader->ucode_data_hash(),
                            pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                            uint64_t(geometry_shader_key.key)};
      if (logged_geom.insert(XXH3_64bits(k, sizeof(k))).second &&
          logged_geom.size() <= 64) {
        REXLOG_INFO(
            "[converter-use] geometry emulation: VS {:016X} PS {:016X} type {} "
            "key {:08X} memexport {} clip-cull {} kill-and {} (native points "
            "{} rects {})",
            k[0], k[1], uint32_t(geometry_shader_key.type),
            geometry_shader_key.key, memexport_used_vertex ? 1 : 0,
            uint32_t(geometry_shader_key.user_clip_plane_cull),
            uint32_t(geometry_shader_key.has_vertex_kill_and),
            NativePointsMode(), NativeRectsMode() ? 1 : 0);
      }
    } else if (spec_pipeline_handle) {
      const bool vs_native =
          spec_pipeline_handle->ns_vs_native.load(std::memory_order_relaxed);
      const bool std_ps_native =
          !pixel_translation ||
          (pixel_translation->is_native_xlate() &&
           spec_pipeline_handle->ns_optimized.load(std::memory_order_relaxed));
      if (!vs_native) cc::CountDraw(cc::kDrawStdConvVS);
      if (!std_ps_native) cc::CountDraw(cc::kDrawStdConvPS);
      if (vs_native && std_ps_native) cc::CountDraw(cc::kDrawStdNative);
    }
  }

  pipeline_cache_->SetupShaderBindingLayoutUserUIDs(*metal_vertex_shader);
  if (metal_pixel_shader) {
    pipeline_cache_->SetupShaderBindingLayoutUserUIDs(*metal_pixel_shader);
  }

  uint32_t used_texture_mask =
      metal_vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (metal_pixel_shader) {
    used_texture_mask |=
        metal_pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  uint32_t texture_request_work_mask = 0;
  if (texture_cache_ && used_texture_mask) {
    texture_request_work_mask =
        texture_cache_->GetUsedTextureRequestWorkMask(used_texture_mask);
  }
  const bool has_texture_request_work =
      texture_cache_ && used_texture_mask && texture_request_work_mask;
  // [draw-prep-lite2] state the check below depends on, taken first.
  const TextureCache::LoadCheckState texture_load_check_state =
      has_texture_request_work ? texture_cache_->GetLoadCheckState()
                               : TextureCache::LoadCheckState();
  const bool may_texture_request_load_data =
      has_texture_request_work &&
      texture_cache_->MayRequestTexturesLoadData(used_texture_mask);

  MTL::RenderPassDescriptor* draw_pass_descriptor =
      GetDrawRenderPassDescriptor(fallback_depth_attachment_required);

  // [prepared-draw-pool] The draw record is built in place: the texture plan and the
  // shared-memory range list below are its own members (no move at the end), and with
  // FABLE2_PREPARED_DRAW_POOL it is a reused scratch whose vectors keep their capacity.
  // [prepared-draw-slim] the record is owned here until SubmitPreparedDraw takes it; on an
  // early return it goes back to the pool.
  struct PreparedDrawLease {
    MetalCommandProcessor* self;
    std::unique_ptr<PreparedDraw> ptr;
    ~PreparedDrawLease() {
      if (ptr) self->ReleasePreparedDraw(std::move(ptr));
    }
  } prepared_draw_lease{this, AcquirePreparedDraw()};
  PreparedDraw& prepared_draw = *prepared_draw_lease.ptr;
  // [prepared-draw-slim] With FABLE2_PREPARED_DRAW_SLIM=0 the fixed arrays, uniforms and
  // dynamic state are built in locals and copied into the record at the end, as before.
  const bool prepared_draw_slim = PreparedDrawSlimEnabled();
  struct PreparedDrawLegacyLocals {
    std::array<VertexBindingRange, 32> vertex_ranges;
    std::array<SharedMemoryRange, 96> shared_memory_hazard_ranges = {};
    UniformBufferInfo uniforms;
    DrawDynamicState dynamic_state;
  };
  std::optional<PreparedDrawLegacyLocals> legacy_locals;
  if (!prepared_draw_slim) {
    legacy_locals.emplace();
  }
  MetalTextureCache::TextureMaterializationPlan& texture_materialization_plan =
      prepared_draw.texture_materialization_plan;
  bool textures_requested_for_draw = false;
  auto request_textures_for_draw = [&](uint64_t& telemetry_counter) -> bool {
    if (!EnsureCommandBuffer()) {
      return false;
    }
    if (texture_materialization_plan.NeedsTextureUpload()) {
      texture_cache_->RequestTexturesWithoutLoading(
          used_texture_mask, &texture_materialization_plan);
    } else if (DrawPrepLite2Enabled()) {
      // [draw-prep-lite2] reuse this draw's load-data check (no second pass).
      hitch::Scope hitch_tex_scope(hitch::kTex);
      texture_cache_->RequestTexturesWithLoadHint(
          used_texture_mask, may_texture_request_load_data,
          texture_load_check_state);
    } else {
      hitch::Scope hitch_tex_scope(hitch::kTex);
      texture_cache_->RequestTextures(used_texture_mask);
    }
    textures_requested_for_draw = true;
    ++telemetry_counter;
    return true;
  };
  std::array<VertexBindingRange, 32>& vertex_ranges =
      prepared_draw_slim ? prepared_draw.vertex_ranges
                         : legacy_locals->vertex_ranges;
  uint32_t vertex_range_count = 0;
  const auto& vb_bindings = vertex_shader->vertex_bindings();
  bool uses_vertex_fetch = ShaderUsesVertexFetch(*vertex_shader);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;
  MTL::RenderStages memexport_write_stages = MTL::RenderStages(0);
  if (memexport_used_vertex) {
    memexport_write_stages =
        (use_geometry_emulation || use_tessellation_emulation ||
         native_points_mesh)
            ? MTL::RenderStages(MTL::RenderStageObject | MTL::RenderStageMesh)
            : MTL::RenderStageVertex;
  }
  if (memexport_used_pixel && is_rasterization_done) {
    memexport_write_stages =
        MTL::RenderStages(NS::UInteger(memexport_write_stages) |
                          NS::UInteger(MTL::RenderStageFragment));
  }
  bool shared_memory_is_uav = memexport_used;
  MTL::ResourceUsage shared_memory_usage =
      shared_memory_is_uav ? (MTL::ResourceUsageRead | MTL::ResourceUsageWrite)
                           : MTL::ResourceUsageRead;
  const bool guest_dma_index_buffer_read =
      !memexport_used &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA;
  const bool shader_primitive_index_load =
      primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA;
  std::array<SharedMemory::Range, kMaxCurrentDrawVertexFetchRanges>
      vertex_fetch_ranges;
  uint32_t vertex_fetch_range_count = 0;
  std::vector<SharedMemory::Range>& current_draw_shared_memory_ranges =
      prepared_draw.materialization_ranges;
  current_draw_shared_memory_ranges.reserve(kMaxCurrentDrawVertexFetchRanges +
                                            memexport_ranges_.size() + 2);
  auto add_current_draw_shared_memory_range =
      [&](DrawMaterializationSource source, uint32_t start, uint32_t length) {
        if (!length) {
          return;
        }
        size_t source_index = static_cast<size_t>(source);
        if (source_index < kDrawMaterializationSourceCount) {
          ++backend_telemetry_.draw_materialization_source_ranges[source_index];
          backend_telemetry_.draw_materialization_source_bytes[source_index] +=
              length;
          if (shared_memory_ && !shared_memory_->IsRangeValid(start, length)) {
            ++backend_telemetry_
                  .draw_materialization_source_invalid_ranges[source_index];
            backend_telemetry_
                .draw_materialization_source_invalid_bytes[source_index] +=
                length;
          }
        }
        current_draw_shared_memory_ranges.push_back({start, length});
      };
  if (texture_cache_ && may_texture_request_load_data) {
    if (!texture_cache_->PrepareTextureMaterialization(
            regs, used_texture_mask, texture_materialization_plan)) {
      return false;
    }
    for (const SharedMemory::Range& range :
         texture_materialization_plan.source_ranges) {
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kTextureSource, range.start, range.length);
    }
  }

  // Sync shared memory before drawing - ensure GPU has latest data
  // This is particularly important for trace playback where memory is
  // written incrementally
  if (shared_memory_) {
    if (!CollectMetalVertexFetchSharedMemoryRanges(
            regs, *vertex_shader, vertex_fetch_ranges.data(),
            uint32_t(vertex_fetch_ranges.size()), &vertex_fetch_range_count,
            true)) {
      return false;
    }
    for (uint32_t i = 0; i < vertex_fetch_range_count; ++i) {
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kVertexFetch, vertex_fetch_ranges[i].start,
          vertex_fetch_ranges[i].length);
    }
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      uint32_t base_bytes = memexport_range.base_address_dwords << 2;
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kMemexport, base_bytes,
          memexport_range.size_bytes);
    }

    auto add_guest_index_range = [&](uint64_t index_base, uint32_t index_count,
                                     xenos::IndexFormat index_format,
                                     const char* label) -> bool {
      uint32_t index_stride = index_format == xenos::IndexFormat::kInt16
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint64_t index_length = uint64_t(index_count) * index_stride;
      if (index_base > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - index_base < index_length) {
        REXLOG_WARN(
            "{} index buffer range out of bounds (base=0x{:08X} size={} "
            "count={})",
            label, static_cast<uint32_t>(index_base), index_length,
            index_count);
        return false;
      }
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kGuestIndex,
          static_cast<uint32_t>(index_base),
          static_cast<uint32_t>(index_length));
      return true;
    };
    if (guest_dma_index_buffer_read &&
        !add_guest_index_range(
            primitive_processing_result.guest_index_base,
            primitive_processing_result.host_draw_vertex_count,
            primitive_processing_result.host_index_format, "guest DMA")) {
      return false;
    }
    if (shader_primitive_index_load &&
        !add_guest_index_range(
            primitive_processing_result.guest_index_base,
            primitive_processing_result.guest_draw_vertex_count,
            regs.Get<reg::VGT_DRAW_INITIATOR>().index_size,
            "shader primitive")) {
      return false;
    }

    for (const auto& binding : vb_bindings) {
      xenos::xe_gpu_vertex_fetch_t vfetch =
          regs.GetVertexFetch(binding.fetch_constant);
      uint32_t buffer_offset = vfetch.address << 2;
      uint32_t buffer_length = vfetch.size << 2;
      VertexBindingRange range;
      range.binding_index = static_cast<uint32_t>(binding.binding_index);
      range.offset = buffer_offset;
      range.length = buffer_length;
      range.stride = binding.stride_words * 4;
      assert_true(vertex_range_count < vertex_ranges.size());
      vertex_ranges[vertex_range_count++] = range;
    }
  }

  // Trace-only (FABLE2_SHMEM_CHECK=1): for vertex/index ranges the shared
  // memory considers valid, compare the GPU-visible shared buffer contents with
  // guest RAM at command-processor time. A mismatch means the GPU would read
  // data that differs from what the guest wrote (stale upload / missed
  // invalidation / not-yet-executed staged upload / GPU-written memexport).
  {
    static const bool shmem_check = [] {
      const char* v = std::getenv("FABLE2_SHMEM_CHECK");
      return v && v[0] == '1';
    }();
    if (shmem_check && shared_memory_ && shared_memory_->GetBuffer()) {
      static uint32_t shmem_check_logged = 0;
      auto* gpu_base =
          static_cast<const uint8_t*>(shared_memory_->GetBuffer()->contents());
      auto check = [&](const char* kind, uint32_t start, uint32_t length) {
        if (!length || !gpu_base || uint64_t(start) + length > SharedMemory::kBufferSize) {
          return;
        }
        if (!shared_memory_->IsRangeValid(start, length)) {
          return;
        }
        auto* guest = static_cast<const uint8_t*>(memory_->TranslatePhysical(start));
        if (!guest) return;
        const uint8_t* gpu = gpu_base + start;
        if (std::memcmp(guest, gpu, length) == 0) return;
        uint32_t first = 0, diff = 0;
        for (uint32_t i = 0; i < length; ++i) {
          if (guest[i] != gpu[i]) { if (!diff) first = i; ++diff; }
        }
        frame_log::Record(kind, frame_log::CurrentFrame(), diff);
        if (shmem_check_logged < 400) {
          ++shmem_check_logged;
          REXLOG_WARN(
              "[shmem-check] frame={} {} start=0x{:08X} len={} first_diff=+{} "
              "diff_bytes={} vs={:016X} ps={:016X} prim={} verts={}",
              frame_log::CurrentFrame(), kind, start, length, first, diff,
              vertex_shader->ucode_data_hash(),
              pixel_shader ? pixel_shader->ucode_data_hash() : 0,
              uint32_t(primitive_processing_result.host_primitive_type),
              primitive_processing_result.host_draw_vertex_count);
        }
      };
      for (uint32_t i = 0; i < vertex_range_count; ++i) {
        check("sc_vb_mismatch", vertex_ranges[i].offset, vertex_ranges[i].length);
      }
      if (guest_dma_index_buffer_read || shader_primitive_index_load) {
        uint32_t index_stride =
            primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4;
        uint32_t count = shader_primitive_index_load
                             ? primitive_processing_result.guest_draw_vertex_count
                             : primitive_processing_result.host_draw_vertex_count;
        check("sc_ib_mismatch", uint32_t(primitive_processing_result.guest_index_base),
              count * index_stride);
      }
    }
  }

  // Trace-only (FABLE2_DRAW_DUMP=<file>): append one fixed-size record per
  // draw for frames in FABLE2_DRAW_DUMP_FRAMES=<first>-<last>. Default off.
  {
    struct DrawDumpConfig {
      FILE* file = nullptr;
      uint64_t first = 0, last = 0;
    };
    static DrawDumpConfig* dump_config = [] {
      const char* path = std::getenv("FABLE2_DRAW_DUMP");
      if (!path || !path[0]) return static_cast<DrawDumpConfig*>(nullptr);
      auto* c = new DrawDumpConfig;
      c->file = std::fopen(path, "wb");
      if (c->file) std::setvbuf(c->file, nullptr, _IOFBF, 16 << 20);
      unsigned long long a = 0, b = 0;
      const char* fr = std::getenv("FABLE2_DRAW_DUMP_FRAMES");
      if (fr && std::sscanf(fr, "%llu-%llu", &a, &b) == 2) {
        c->first = a; c->last = b;
      } else {
        c->first = 0; c->last = UINT64_MAX;
      }
      REXLOG_INFO("[draw-dump] {} frames {}-{} ok={}", path, c->first, c->last,
                  c->file != nullptr);
      return c;
    }();
    const uint64_t dump_frame = frame_log::CurrentFrame();
    if (dump_config && dump_config->file && dump_frame >= dump_config->first &&
        dump_frame <= dump_config->last) {
#pragma pack(push, 1)
      struct DrawDumpRecord {
        uint32_t magic;            // 'FDD1'
        uint32_t frame;
        uint32_t draw_index;       // within frame
        uint32_t prim_guest, prim_host;
        uint64_t vs_hash, ps_hash, vs_mod, ps_mod;
        uint32_t guest_vertex_count, host_vertex_count;
        uint32_t index_buffer_type, index_size;  // VGT index_size (0=16,1=32)
        uint32_t guest_index_base, vgt_dma_size, vgt_indx_offset;
        uint32_t idx_min_be, idx_max_be, idx_min_le, idx_max_le;
        uint64_t ib_hash_guest, ib_hash_gpu;
        uint32_t vb_count;
        uint32_t vb_addr[4], vb_size[4], vb_stride[4];
        uint64_t vb_hash_guest[4], vb_hash_gpu[4];
        uint32_t vb_valid_mask;
        uint64_t vsc_hash;         // all 256 VS float constants
        float vsc_maxabs64;        // max |c| in constants 0..63
        uint32_t vsc_nan64, vsc_huge64;  // NaN/Inf count, |c|>1e6 count
        float vsc64[64 * 4];       // first 64 VS float constants
        uint32_t blend0, depthctl, colorctl, colormask, su_sc_mode;
        uint32_t flags;            // bit0 memexport vs, bit1 geom emu, bit2 has ps
      };
#pragma pack(pop)
      static uint64_t last_frame = UINT64_MAX;
      static uint32_t draw_index = 0;
      if (dump_frame != last_frame) { last_frame = dump_frame; draw_index = 0; }
      DrawDumpRecord r = {};
      r.magic = 0x31444446;
      r.frame = uint32_t(dump_frame);
      r.draw_index = draw_index++;
      r.prim_guest = uint32_t(primitive_processing_result.guest_primitive_type);
      r.prim_host = uint32_t(primitive_processing_result.host_primitive_type);
      r.vs_hash = vertex_shader->ucode_data_hash();
      r.ps_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
      r.vs_mod = vertex_shader_modification.value;
      r.ps_mod = pixel_shader_modification.value;
      r.guest_vertex_count = primitive_processing_result.guest_draw_vertex_count;
      r.host_vertex_count = primitive_processing_result.host_draw_vertex_count;
      r.index_buffer_type = uint32_t(primitive_processing_result.index_buffer_type);
      r.index_size = uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().index_size);
      r.guest_index_base = uint32_t(primitive_processing_result.guest_index_base);
      r.vgt_dma_size = regs[XE_GPU_REG_VGT_DMA_SIZE];
      r.vgt_indx_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
      const uint8_t* gpu_base =
          (shared_memory_ && shared_memory_->GetBuffer())
              ? static_cast<const uint8_t*>(shared_memory_->GetBuffer()->contents())
              : nullptr;
      auto hash_range = [&](uint32_t start, uint32_t length, uint64_t& guest_hash,
                            uint64_t& gpu_hash) {
        if (!length || uint64_t(start) + length > SharedMemory::kBufferSize) return;
        uint32_t n = std::min<uint32_t>(length, 1u << 20);
        if (auto* g = static_cast<const uint8_t*>(memory_->TranslatePhysical(start))) {
          guest_hash = XXH3_64bits(g, n);
        }
        if (gpu_base) gpu_hash = XXH3_64bits(gpu_base + start, n);
      };
      if (primitive_processing_result.index_buffer_type !=
          PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
        const bool idx32 = r.index_size != 0;
        const uint32_t count = primitive_processing_result.guest_draw_vertex_count;
        const uint32_t bytes = count * (idx32 ? 4u : 2u);
        if (uint64_t(r.guest_index_base) + bytes <= SharedMemory::kBufferSize) {
          auto* g = static_cast<const uint8_t*>(
              memory_->TranslatePhysical(r.guest_index_base));
          if (g && count) {
            uint32_t mn_be = UINT32_MAX, mx_be = 0, mn_le = UINT32_MAX, mx_le = 0;
            for (uint32_t i = 0; i < count; ++i) {
              uint32_t be, le;
              if (idx32) {
                const uint8_t* q = g + i * 4;
                be = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                     (uint32_t(q[2]) << 8) | q[3];
                le = (uint32_t(q[3]) << 24) | (uint32_t(q[2]) << 16) |
                     (uint32_t(q[1]) << 8) | q[0];
              } else {
                const uint8_t* q = g + i * 2;
                be = (uint32_t(q[0]) << 8) | q[1];
                le = (uint32_t(q[1]) << 8) | q[0];
              }
              mn_be = std::min(mn_be, be); mx_be = std::max(mx_be, be);
              mn_le = std::min(mn_le, le); mx_le = std::max(mx_le, le);
            }
            r.idx_min_be = mn_be; r.idx_max_be = mx_be;
            r.idx_min_le = mn_le; r.idx_max_le = mx_le;
          }
          hash_range(r.guest_index_base, bytes, r.ib_hash_guest, r.ib_hash_gpu);
        }
      }
      r.vb_count = vertex_range_count;
      for (uint32_t i = 0; i < vertex_range_count && i < 4; ++i) {
        r.vb_addr[i] = vertex_ranges[i].offset;
        r.vb_size[i] = vertex_ranges[i].length;
        r.vb_stride[i] = vertex_ranges[i].stride;
        hash_range(vertex_ranges[i].offset, vertex_ranges[i].length,
                   r.vb_hash_guest[i], r.vb_hash_gpu[i]);
        if (shared_memory_ && vertex_ranges[i].length &&
            shared_memory_->IsRangeValid(vertex_ranges[i].offset,
                                         vertex_ranges[i].length)) {
          r.vb_valid_mask |= 1u << i;
        }
      }
      const uint32_t* vsc = &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X];
      r.vsc_hash = XXH3_64bits(vsc, 256 * 4 * sizeof(uint32_t));
      std::memcpy(r.vsc64, vsc, sizeof(r.vsc64));
      for (uint32_t i = 0; i < 64 * 4; ++i) {
        float f = r.vsc64[i];
        if (!std::isfinite(f)) { ++r.vsc_nan64; continue; }
        float a = std::fabs(f);
        if (a > 1e6f) ++r.vsc_huge64;
        r.vsc_maxabs64 = std::max(r.vsc_maxabs64, a);
      }
      r.blend0 = regs[XE_GPU_REG_RB_BLENDCONTROL0];
      r.depthctl = regs[XE_GPU_REG_RB_DEPTHCONTROL];
      r.colorctl = regs[XE_GPU_REG_RB_COLORCONTROL];
      r.colormask = regs[XE_GPU_REG_RB_COLOR_MASK];
      r.su_sc_mode = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
      r.flags = (memexport_used_vertex ? 1u : 0u) |
                (use_geometry_emulation ? 2u : 0u) | (pixel_shader ? 4u : 0u);
      std::fwrite(&r, sizeof(r), 1, dump_config->file);
      static uint64_t flushed_frame = 0;
      if (dump_frame != flushed_frame && (dump_frame & 63) == 0) {
        flushed_frame = dump_frame;
        std::fflush(dump_config->file);
      }
      if (dump_frame == dump_config->last) std::fflush(dump_config->file);
    }
  }

  const bool current_draw_has_invalid_shared_memory =
      shared_memory_ && !current_draw_shared_memory_ranges.empty() &&
      AnySharedMemoryRangeInvalid(
          current_draw_shared_memory_ranges.data(),
          static_cast<uint32_t>(current_draw_shared_memory_ranges.size()));
  if (shared_memory_ && !current_draw_shared_memory_ranges.empty()) {
    ++backend_telemetry_.draw_materialization_per_draw_requests;
    if (current_draw_has_invalid_shared_memory) {
      ++backend_telemetry_.draw_materialization_per_draw_invalid_requests;
    } else {
      ++backend_telemetry_.draw_materialization_per_draw_resident_skips;
    }
  }

  if (has_texture_request_work && !textures_requested_for_draw) {
    const bool texture_request_started_with_active_encoder =
        current_render_encoder_ != nullptr;
    uint64_t& texture_request_counter =
        texture_request_started_with_active_encoder
            ? backend_telemetry_.texture_requests_after_encoder_begin
            : backend_telemetry_.texture_requests_before_encoder;
    if (!request_textures_for_draw(texture_request_counter)) {
      return false;
    }
  }

  UniformBufferInfo& uniforms =
      prepared_draw_slim ? prepared_draw.uniforms : legacy_locals->uniforms;
  DrawDynamicState& draw_dynamic_state =
      prepared_draw_slim ? prepared_draw.dynamic_state
                         : legacy_locals->dynamic_state;
  const bool prepare_uniforms = constant_buffer_pool_ && shared_memory_;
  if (!prepare_uniforms && prepared_draw_slim) {
    uniforms = {};  // the slim reset leaves them to PrepareDrawConstants
  }
  if (prepare_uniforms) {
    if (!draw_pass_descriptor) {
      REXLOG_ERROR("IssueDraw: no render pass descriptor for draw constants");
      return false;
    }
    if (!EnsureCommandBuffer()) {
      return false;
    }
    miplod_ps_mod_ = pixel_shader ? pixel_shader_modification.value : 0;
    if (!PrepareDrawConstants(regs, vertex_shader, pixel_shader,
                              metal_vertex_shader, metal_pixel_shader,
                              shared_memory_is_uav, is_rasterization_done,
                              primitive_processing_result, used_texture_mask,
                              normalized_color_mask, draw_pass_descriptor,
                              uniforms, draw_dynamic_state)) {
      return false;
    }
  }

  PreparedIndexBuffer prepared_guest_dma_index_buffer;
  if (memexport_used &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    if (!PrepareGuestDMAIndexBufferForMemexport(
            primitive_processing_result, prepared_guest_dma_index_buffer)) {
      return false;
    }
  }

  std::array<SharedMemoryRange, 96>& shared_memory_hazard_ranges =
      prepared_draw_slim ? prepared_draw.shared_memory_hazard_ranges
                         : legacy_locals->shared_memory_hazard_ranges;
  uint32_t shared_memory_hazard_range_count = 0;
  auto add_shared_memory_hazard_range = [&](uint32_t start, uint32_t length) {
    if (!length || shared_memory_hazard_range_count >=
                       shared_memory_hazard_ranges.size()) {
      return;
    }
    shared_memory_hazard_ranges[shared_memory_hazard_range_count++] =
        SharedMemoryRange{start, length};
  };
  for (uint32_t i = 0; i < vertex_range_count; ++i) {
    add_shared_memory_hazard_range(vertex_ranges[i].offset,
                                   vertex_ranges[i].length);
  }
  if (guest_dma_index_buffer_read || shader_primitive_index_load) {
    const xenos::IndexFormat index_format =
        shader_primitive_index_load
            ? regs.Get<reg::VGT_DRAW_INITIATOR>().index_size
            : primitive_processing_result.host_index_format;
    uint32_t index_stride = index_format == xenos::IndexFormat::kInt16
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint32_t index_count =
        shader_primitive_index_load
            ? primitive_processing_result.guest_draw_vertex_count
            : primitive_processing_result.host_draw_vertex_count;
    uint64_t index_length = uint64_t(index_count) * index_stride;
    if (index_length <= SharedMemory::kBufferSize) {
      add_shared_memory_hazard_range(
          static_cast<uint32_t>(primitive_processing_result.guest_index_base),
          static_cast<uint32_t>(index_length));
    }
  }
  if (NS::UInteger(memexport_write_stages)) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      add_shared_memory_hazard_range(memexport_range.base_address_dwords << 2,
                                     memexport_range.size_bytes);
    }
  }
  MTL::RenderStages shared_memory_consumer_stages = MTL::RenderStages(0);
  if (vertex_range_count || guest_dma_index_buffer_read ||
      shader_primitive_index_load) {
    shared_memory_consumer_stages =
        (use_geometry_emulation || use_tessellation_emulation ||
         native_points_mesh)
            ? MTL::RenderStages(MTL::RenderStageObject | MTL::RenderStageMesh)
            : MTL::RenderStageVertex;
  }
  shared_memory_consumer_stages =
      MTL::RenderStages(NS::UInteger(shared_memory_consumer_stages) |
                        NS::UInteger(memexport_write_stages));

  // [shadows-native] diagnostic: one line per draw in the trace window, with
  // the bound render targets and every fetched texture (FABLE2_SHADOW_TRACE).
  if (render_target_cache_ && MetalRenderTargetCache::ShadowTraceActive()) {
    std::string tex;
    uint32_t remaining = used_texture_mask;
    uint32_t ti;
    while (rex::bit_scan_forward(remaining, &ti)) {
      remaining &= ~(uint32_t(1) << ti);
      const auto fetch = regs.GetTextureFetch(ti);
      const uint32_t base = uint32_t(fetch.base_address << 12);
      tex += fmt::format(
          " T{}=0x{:08X}/fmt{}/dim{}/{}x{}/p{}/m0x{:08X}/mip{}-{}/bias{}", ti,
          base, uint32_t(fetch.format), uint32_t(fetch.dimension),
          uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1,
          uint32_t(fetch.pitch) * 32, uint32_t(fetch.mip_address << 12),
          uint32_t(fetch.mip_min_level), uint32_t(fetch.mip_max_level),
          int32_t(fetch.lod_bias));
    }
    tex += fmt::format(" ib=0x{:08X} n={}",
                       index_buffer_info ? index_buffer_info->guest_base : 0u,
                       index_count);
    REXLOG_INFO("[shadow-trace] f{} draw {} vs={:016X} ps={:016X} zc={:08X} "
                "cm={:08X} sc={:08X}-{:08X} vp={}x{}{}{}",
                frame_log::CurrentFrame(),
                render_target_cache_->DescribeCurrentTargetsForTrace(),
                vertex_shader->ucode_data_hash(),
                pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_COLOR_MASK],
                regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
                regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) * 2.0f,
                regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) * -2.0f, tex,
                hle_census::Enabled()
                    ? fmt::format(" site={} prim={} n={}", census_draw_site_,
                                  uint32_t(primitive_type), index_count)
                    : std::string());
  }
  const uint32_t trace_texture_base = REXCVAR_GET(metal_draw_trace_texture_base);
  if (REXCVAR_GET(metal_capture_labels) || trace_texture_base) {
    // Retain the identity with queued draws, before guest shader state changes.
    prepared_draw.capture_label = fmt::format(
        "Guest VS={:016X} PS={:016X}", vertex_shader->ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    uint32_t remaining = used_texture_mask;
    uint32_t texture_index;
    bool trace_texture_bound = false;
    std::string trace_filters;
    while (rex::bit_scan_forward(remaining, &texture_index)) {
      remaining &= ~(uint32_t(1) << texture_index);
      const auto fetch = regs.GetTextureFetch(texture_index);
      trace_texture_bound |= trace_texture_base &&
                             uint32_t(fetch.base_address << 12) == trace_texture_base;
      if (trace_texture_base) {
        trace_filters += fmt::format(" T{}filter={}/{}/{} mip={}-{} bias={} aniso={}",
                                     texture_index, uint32_t(fetch.min_filter),
                                     uint32_t(fetch.mag_filter), uint32_t(fetch.mip_filter),
                                     uint32_t(fetch.mip_min_level), uint32_t(fetch.mip_max_level),
                                     int32_t(fetch.lod_bias), uint32_t(fetch.aniso_filter));
      }
      prepared_draw.capture_label += fmt::format(
          " T{}={:08X}/{:08X}", texture_index,
          uint32_t(fetch.base_address << 12),
          uint32_t(fetch.mip_address << 12));
    }
    if (trace_texture_bound) {
      static std::vector<std::string> traced_bindings;
      const auto trace_identity = fmt::format(
          "{}{} depth=0x{:08X} color=0x{:08X} bias={}/{} cull={}",
          prepared_draw.capture_label, trace_filters,
          (*register_file_)[XE_GPU_REG_RB_DEPTHCONTROL],
          (*register_file_)[XE_GPU_REG_RB_COLORCONTROL],
          draw_dynamic_state.depth_bias_constant, draw_dynamic_state.depth_bias_slope,
          uint32_t(draw_dynamic_state.cull_mode));
      if (traced_bindings.size() < 32 &&
          std::find(traced_bindings.begin(), traced_bindings.end(),
                    trace_identity) == traced_bindings.end()) {
        traced_bindings.push_back(trace_identity);
        REXLOG_INFO("Metal texture draw trace: {}", trace_identity);
      }
    }
  }
  if (pixel_shader && MetalTextureCache::MipLodTraceEnabled()) {
    texture_cache_->MipLodTraceNoteDraw(*pixel_shader,
                                        vertex_shader->ucode_data_hash(),
                                        uint32_t(primitive_type));
  }
  // FABLE2_NS_SPECIALIZE (metal_ns_specialize.h, docs/native-shaders.md
  // "Hot-shader specialization"): the constant bytes bound for the pixel
  // stage are final here. Learn mode records them; a pixel shader with a
  // specialized variant binds its specialized pipeline only when every guarded
  // dword equals the baked value, else the normal pipeline.
  if (pipeline && pixel_translation && !use_fallback_ps && prepare_uniforms) {
    static const bool spec_on = NativeShadersMode() && ns_spec::Enabled();
    static const bool learn_on = NativeShadersMode() && ns_spec::LearnMode();
    static const bool verify_on = ns_spec::VerifyMode();
    static const bool auto_on = spec_on && ns_spec::AutoLearnEnabled();
    static const uint32_t auto_every = ns_spec::AutoLearnSampleEvery();
    const ns_spec::Variant* variant =
        spec_on ? pixel_translation->spec_variant() : nullptr;
    // A demoted variant is never bound again this session.
    if (variant && variant->demoted.load(std::memory_order_relaxed)) {
      variant = nullptr;
    }
    // FABLE2_NATIVE_XLATE: a natively translated shader replaces the
    // converter library its variant was specialized from (specialization of
    // native shaders is future work, docs/native-translator.md).
    const bool pixel_native = pixel_translation->is_native_xlate();
    if (pixel_native && variant && !variant->native) {
      variant = nullptr;
    }
    // FABLE2_NS_SPECIALIZE_AUTOLEARN: candidate shaders without learn data
    // sample every Nth draw until their statistics converge.
    ns_spec::AutoLearn* auto_learn = nullptr;
    if (auto_on && !variant) {
      auto_learn = pixel_translation->spec_auto();
      if (!auto_learn && pixel_translation->spec_needs_learn().load(
                             std::memory_order_relaxed)) {
        auto_learn = ns_spec::NewAutoLearn(
            pixel_translation->plain_key(),
            pixel_translation->shader().ucode_data_hash(),
            pixel_translation->modification());
        pixel_translation->set_spec_auto(auto_learn);
      }
      if (auto_learn && !ns_spec::AutoLearnWantsSample(*auto_learn,
                                                       auto_every)) {
        auto_learn = nullptr;
      }
    }
    if (variant || learn_on || auto_learn) {
      ns_spec::DrawPayloads payloads;
      const uint32_t pixel_cbvs = uniforms.active_cbv_masks[kStagePixel];
      auto set_payload = [&](uint32_t kind, uint32_t slot,
                             const std::vector<uint8_t>& bytes,
                             size_t logical_size) {
        if ((pixel_cbvs & (uint32_t(1) << slot)) &&
            bytes.size() >= logical_size) {
          payloads.data[kind] =
              reinterpret_cast<const uint32_t*>(bytes.data());
          payloads.dwords[kind] = uint32_t(logical_size / sizeof(uint32_t));
        }
      };
      set_payload(ns_spec::kSystem, kCbvSlotSystem, current_payload_system_,
                  sizeof(DxbcShaderTranslator::SystemConstants));
      set_payload(ns_spec::kBoolLoop, kCbvSlotBoolLoop,
                  current_payload_bool_loop_, (8 + 32) * sizeof(uint32_t));
      set_payload(ns_spec::kFetch, kCbvSlotFetch,
                  current_payload_fetch_[kStagePixel],
                  xenos::kTextureFetchConstantCount * 6 * sizeof(uint32_t));
      if (learn_on) {
        ns_spec::LearnNote(pixel_translation, pixel_translation->plain_key(),
                           pixel_translation->shader().ucode_data_hash(),
                           pixel_translation->modification(), payloads);
      }
      if (auto_learn && ns_spec::AutoLearnSample(*auto_learn, payloads)) {
        pipeline_cache_->RequestRespecialization(pixel_translation);
      }
      if (variant) {
        ns_spec::Counters& counters = ns_spec::GetCounters();
        const uint64_t checks =
            counters.checks.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool matched = variant->Matches(payloads);
        if (!matched) {
          ns_spec::NoteMiss(*variant, payloads);
        }
        if (ns_spec::CheckDemotion(*variant, !matched) && auto_on &&
            !ns_spec::RecordDemotion(pixel_translation->plain_key(),
                                     *variant)) {
          // No learn data behind the variant (seeded): learn it now, for
          // the next session.
          pixel_translation->set_spec_auto(ns_spec::AutoLearnAfterDemotion(
              pixel_translation->spec_auto(), *variant,
              pixel_translation->plain_key()));
        }
        if (matched) {
          variant->hits.fetch_add(1, std::memory_order_relaxed);
          MTL::RenderPipelineState* spec_pipeline = nullptr;
          if (spec_pipeline_handle) {
            spec_pipeline = spec_pipeline_handle->ns_spec_state.load(
                std::memory_order_acquire);
            if (!spec_pipeline) {
              pipeline_cache_->RequestSpecPipeline(spec_pipeline_handle);
            }
          } else {
            const std::shared_ptr<MetalPipelineCache::NsEmuSwap>* emu =
                tessellation_pipeline_state
                    ? &tessellation_pipeline_state->ns_emu
                    : (geometry_pipeline_state
                           ? &geometry_pipeline_state->ns_emu
                           : nullptr);
            if (emu && *emu) {
              spec_pipeline = (*emu)->spec.load(std::memory_order_acquire);
              if (!spec_pipeline) {
                pipeline_cache_->RequestSpecEmuPipeline(*emu);
              }
            }
          }
          if (spec_pipeline) {
            pipeline = spec_pipeline;
            counters.hits.fetch_add(1, std::memory_order_relaxed);
            if (verify_on) {
              // Independent re-check against the guest registers and the
              // system constants (catches stale bound constant bytes).
              bool same = true;
              const uint32_t* sys =
                  reinterpret_cast<const uint32_t*>(&system_constants_);
              for (uint32_t k = 0; k < ns_spec::kKindCount && same; ++k) {
                for (size_t i = 0; i < variant->index[k].size(); ++i) {
                  const uint32_t idx = variant->index[k][i];
                  uint32_t live;
                  if (k == ns_spec::kSystem) {
                    live = sys[idx];
                  } else if (k == ns_spec::kBoolLoop) {
                    live = regs.values
                        [XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + idx];
                  } else {
                    live = regs.values
                        [XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + idx];
                  }
                  if ((live & variant->mask[k][i]) != variant->value[k][i]) {
                    same = false;
                    break;
                  }
                }
              }
              counters.verified.fetch_add(1, std::memory_order_relaxed);
              if (!same) {
                const uint64_t n =
                    counters.misbinds.fetch_add(1, std::memory_order_relaxed) +
                    1;
                if (n <= 8) {
                  REXLOG_WARN(
                      "[ns-spec] verify: PS {:016X}/{:X} guarded dwords match "
                      "the bound constants but not the registers (#{})",
                      variant->ucode_hash, variant->modification, n);
                }
              }
            }
          } else {
            counters.hit_no_pipeline.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          const uint64_t m =
              variant->misses.fetch_add(1, std::memory_order_relaxed);
          counters.misses.fetch_add(1, std::memory_order_relaxed);
          // FABLE2_NS_SPECIALIZE_LOG_MISSES=N (default 1): the first N
          // misses of each variant name the dwords that differ.
          static const uint32_t log_misses = [] {
            const char* v = std::getenv("FABLE2_NS_SPECIALIZE_LOG_MISSES");
            return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 1u;
          }();
          if (m < log_misses) {
            std::string diff;
            for (uint32_t k = 0; k < ns_spec::kKindCount; ++k) {
              for (size_t i = 0; i < variant->index[k].size(); ++i) {
                const uint32_t idx = variant->index[k][i];
                if (!payloads.data[k] || idx >= payloads.dwords[k]) {
                  diff += fmt::format(" {}:{}=unbound", k, idx);
                } else if ((payloads.data[k][idx] & variant->mask[k][i]) !=
                           variant->value[k][i]) {
                  diff += fmt::format(" {}:{}={:08X}!={:08X}/{:08X}", k, idx,
                                      payloads.data[k][idx],
                                      variant->value[k][i],
                                      variant->mask[k][i]);
                }
              }
            }
            REXLOG_INFO("[ns-spec] miss PS {:016X}/{:X}:{}",
                        variant->ucode_hash, variant->modification, diff);
          }
        }
        if ((checks & 8191) == 0) {
          ns_spec::LogCounters(false);
        }
      }
    }
  }
  prepared_draw.trace_ps =
      pass_trace::ShaderKeysEnabled() && pixel_shader
          ? pixel_shader->ucode_data_hash()
          : 0;
  prepared_draw.pipeline = pipeline;
  prepared_draw.tessellation_pipeline_state = tessellation_pipeline_state;
  prepared_draw.geometry_pipeline_state = geometry_pipeline_state;
  prepared_draw.primitive_processing_result = primitive_processing_result;
  if (!prepared_draw_slim) {
    prepared_draw.uniforms = uniforms;
    prepared_draw.dynamic_state = draw_dynamic_state;
    prepared_draw.vertex_ranges = vertex_ranges;
    prepared_draw.shared_memory_hazard_ranges = shared_memory_hazard_ranges;
  }
  prepared_draw.prepared_guest_dma_index_buffer =
      prepared_guest_dma_index_buffer;
  if (index_buffer_info) {
    prepared_draw.index_buffer_info = *index_buffer_info;
    prepared_draw.has_index_buffer_info = true;
  }
  prepared_draw.has_vertex_bindings = !vb_bindings.empty();
  if (!DrawPrepLiteEnabled()) {
    prepared_draw.vertex_bindings = vb_bindings;
  }
  prepared_draw.vertex_range_count = vertex_range_count;
  prepared_draw.has_invalid_shared_memory =
      current_draw_has_invalid_shared_memory;
  prepared_draw.shared_memory_hazard_range_count =
      shared_memory_hazard_range_count;
  prepared_draw.shared_memory_consumer_stages = shared_memory_consumer_stages;
  prepared_draw.memexport_ranges = memexport_ranges_;
  prepared_draw.memexport_write_stages = memexport_write_stages;
  prepared_draw.shared_memory_usage = shared_memory_usage;
  prepared_draw.render_target_key = render_target_key;
  prepared_draw.texture_resource_set = current_bindless_texture_resource_set_;
  prepared_draw.texture_upload_needed =
      texture_materialization_plan.NeedsTextureUpload();
  prepared_draw.use_tessellation_emulation = use_tessellation_emulation;
  prepared_draw.use_geometry_emulation = use_geometry_emulation;
  prepared_draw.native_tess = native_tess;
  prepared_draw.native_tess_cpu = native_tess_cpu;
  if (native_tess_cpu) {
    prepared_draw.tess_merged_indices = native_tess_merged_buffer;
    prepared_draw.tess_merged_offset = native_tess_merged_offset;
    prepared_draw.tess_merged_count = native_tess_merged_count;
    if (!native_tess_merged_buffer) {
      prepared_draw.tess_runs.assign(native_tess_runs_scratch_.begin(),
                                     native_tess_runs_scratch_.end());
    }
  }
  prepared_draw.native_points = native_points;
  prepared_draw.native_geometry_vertices = native_geometry_vertices;
  prepared_draw.native_points_mesh = native_points_mesh;
  prepared_draw.tess_factor_buffer = native_tess_factor_buffer;
  prepared_draw.tess_factor_offset = native_tess_factor_offset;
  prepared_draw.tess_patch_count = native_tess_patches;
  prepared_draw.shared_memory_is_uav = shared_memory_is_uav;
  prepared_draw.memexport_used = memexport_used;
  prepared_draw.uses_vertex_fetch = uses_vertex_fetch;
  prepared_draw.prepare_uniforms = prepare_uniforms;
  prepared_draw.fallback_depth_attachment_required =
      fallback_depth_attachment_required;
  prepared_draw.may_texture_request_load_data = may_texture_request_load_data;
  prepared_draw.has_pending_draw_pass_transfers =
      render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers();
  draw_skip_reason_ = "ds_submitted";
  return SubmitPreparedDraw(std::move(prepared_draw_lease.ptr));
}

bool MetalCommandProcessor::PrepareDrawConstants(
    const RegisterFile& regs, Shader* vertex_shader, Shader* pixel_shader,
    MetalShader* metal_vertex_shader, MetalShader* metal_pixel_shader,
    bool shared_memory_is_uav, bool is_rasterization_done,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    uint32_t used_texture_mask, uint32_t normalized_color_mask,
    MTL::RenderPassDescriptor* render_pass_descriptor,
    UniformBufferInfo& uniforms_out, DrawDynamicState& dynamic_state_out) {
  const bool lite2 = DrawPrepLite2Enabled();
  // Determine primitive type characteristics
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);

  // Get viewport info for NDC transform. Use the actual RT0 dimensions
  // when available so system constants match the current render target.
  uint32_t vp_width = 1;
  uint32_t vp_height = 1;
  GetActiveRenderTargetSize(render_pass_descriptor, render_target_cache_.get(),
                            1280, 720, vp_width, vp_height);
  draw_util::ViewportInfo viewport_info;
  auto depth_control = draw_util::GetNormalizedDepthControl(regs);
  auto dynamic_depth_control = depth_control;
  if (!is_rasterization_done) {
    dynamic_depth_control.value = 0;
  }
  constexpr uint32_t kViewportBoundsMax = 32767;
  bool host_render_targets_used = true;
  bool convert_z_to_float24 = host_render_targets_used &&
                              REXCVAR_GET(depth_float24_convert_in_pixel_shader);
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  // [effects-native] native-resolution effect targets are drawn at 1x.
  const bool pass_native_scale =
      render_target_cache_ && render_target_cache_->IsCurrentPassNativeScale();
  if (pass_native_scale) {
    draw_resolution_scale_x = 1;
    draw_resolution_scale_y = 1;
  }
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      draw_resolution_scale_x, draw_resolution_scale_y,
      (texture_cache_ && !pass_native_scale)
          ? texture_cache_->draw_resolution_scale_x_divisor()
          : divisors::MagicDiv(1),
      (texture_cache_ && !pass_native_scale)
          ? texture_cache_->draw_resolution_scale_y_divisor()
          : divisors::MagicDiv(1),
      true, kViewportBoundsMax, kViewportBoundsMax, false, depth_control,
      convert_z_to_float24, host_render_targets_used,
      pixel_shader && pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);
  draw_util::GetHostViewportInfo(&gviargs, viewport_info);

  // Apply per-draw viewport and scissor so the Metal viewport
  // matches the guest viewport computed by draw_util.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // draw_resolution_scale_x/y already computed above for viewport.
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // [rt-shrink] FABLE2_RT_SHRINK_VERIFY: the textures must reach the rows
  // this draw can rasterize (scissor and, with clipping, the viewport).
  if (render_target_cache_) {
    const double vp_bottom = std::max(
        0.0, std::ceil(double(viewport_info.xy_offset[1]) +
                       double(viewport_info.xy_extent[1])));
    render_target_cache_->RtShrinkCheckDraw(
        scissor.offset[1] + scissor.extent[1],
        uint32_t(std::min(vp_bottom, 1.0e9)),
        regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable != 0);
  }
  // Clamp scissor to actual render target bounds (Metal requires this).
  ClampScissorToBounds(scissor, vp_width, vp_height);

  MTL::Viewport mtl_viewport;
  mtl_viewport.originX = static_cast<double>(viewport_info.xy_offset[0]);
  mtl_viewport.originY = static_cast<double>(viewport_info.xy_offset[1]);
  mtl_viewport.width = static_cast<double>(viewport_info.xy_extent[0]);
  mtl_viewport.height = static_cast<double>(viewport_info.xy_extent[1]);
  mtl_viewport.znear = viewport_info.z_min;
  mtl_viewport.zfar = viewport_info.z_max;

  MTL::ScissorRect mtl_scissor;
  mtl_scissor.x = scissor.offset[0];
  mtl_scissor.y = scissor.offset[1];
  mtl_scissor.width = scissor.extent[0];
  mtl_scissor.height = scissor.extent[1];
  dynamic_state_out.viewport = mtl_viewport;
  dynamic_state_out.scissor = mtl_scissor;
  dynamic_state_out.viewport_info = viewport_info;
  dynamic_state_out.depth_control = dynamic_depth_control;
  dynamic_state_out.primitive_polygonal = primitive_polygonal;
  dynamic_state_out.rasterization_enabled = is_rasterization_done;

  // Update full system constants from GPU registers.
  // normalized_color_mask was already computed above for render target update.
  UpdateSystemConstantValues(
      shared_memory_is_uav, primitive_polygonal,
      primitive_processing_result.line_loop_closing_index,
      primitive_processing_result.host_shader_index_endian, viewport_info,
      used_texture_mask, depth_control, normalized_color_mask);

  // Diagnostic (FABLE2_DIAG_TEX_VS=<vs hash hex>): log the texture fetch
  // constants and the resolution-scaled flag of draws with this vertex shader,
  // first 64 draws of every 120th frame, or of frame FABLE2_DIAG_TEX_FRAME=N.
  // Default off (docs/lake-water-20261014.md).
  {
    static const uint64_t diag_tex_vs = [] {
      const char* v = std::getenv("FABLE2_DIAG_TEX_VS");  // getenv-ok: once
      return v ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    if (diag_tex_vs && vertex_shader->ucode_data_hash() == diag_tex_vs &&
        texture_cache_) {
      static uint64_t diag_frame = UINT64_MAX;
      static uint32_t diag_count = 0;
      const uint64_t f = frame_log::CurrentFrame();
      if (f != diag_frame) { diag_frame = f; diag_count = 0; }
      static const uint64_t diag_tex_frame = [] {
        const char* v = std::getenv("FABLE2_DIAG_TEX_FRAME");  // getenv-ok: once
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
      }();
      if ((diag_tex_frame ? f == diag_tex_frame : (f % 120) == 0) &&
          diag_count++ < 64) {
        uint32_t m = used_texture_mask, ti;
        while (rex::bit_scan_forward(m, &ti)) {
          m &= ~(uint32_t(1) << ti);
          const uint32_t* fc =
              &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6];
          REXLOG_INFO(
              "[diag-tex] frame {} vs {:016X} ps {:016X} fetch {} "
              "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} scaled {}",
              f, vertex_shader->ucode_data_hash(),
              pixel_shader ? pixel_shader->ucode_data_hash() : 0ull, ti, fc[0],
              fc[1], fc[2], fc[3], fc[4], fc[5],
              int(texture_cache_->IsActiveTextureResolutionScaled(ti)));
        }
      }
    }
  }

  // [mip-lod] bound the mip levels this draw's fetches can read
  // (metal_miplod_proof.h); sets the pixel-stage view levels used below.
  {
    MetalTextureCache::MipLodDrawInputs mi;
    const DxbcShaderTranslator::Modification pixel_shader_modification(
        miplod_ps_mod_);
    mi.vertex_shader = vertex_shader;
    mi.pixel_shader = pixel_shader;
    mi.pixel_samplers =
        pixel_shader ? &static_cast<MetalShader*>(pixel_shader)
                            ->GetSamplerBindingsAfterTranslation()
                     : nullptr;
    mi.interpolator_mask = pixel_shader_modification.pixel.interpolator_mask;
    mi.interpolators_centroid =
        pixel_shader_modification.pixel.interpolators_centroid;
    mi.param_gen = pixel_shader_modification.pixel.param_gen_enable != 0;
    mi.param_gen_interpolator =
        pixel_shader_modification.pixel.param_gen_interpolator;
    mi.plain_vertex =
        primitive_processing_result.host_vertex_shader_type ==
            Shader::HostVertexShaderType::kVertex &&
        !primitive_processing_result.IsTessellated();
    mi.primitive = primitive_processing_result.guest_primitive_type;
    mi.primitive_reset =
        primitive_processing_result.host_primitive_reset_enabled;
    mi.vertex_count = primitive_processing_result.guest_draw_vertex_count;
    mi.index_type = uint32_t(primitive_processing_result.index_buffer_type);
    mi.index_address = primitive_processing_result.guest_index_base;
    mi.index_32bit = primitive_processing_result.host_index_format ==
                     xenos::IndexFormat::kInt32;
    mi.index_endian = uint32_t(system_constants_.vertex_index_endian);
    mi.index_offset = system_constants_.vertex_index_offset;
    mi.index_min = system_constants_.vertex_index_min;
    mi.index_max = system_constants_.vertex_index_max;
    mi.line_loop_closing_index = system_constants_.line_loop_closing_index;
    mi.pixels_per_x = double(viewport_info.ndc_scale[0]) *
                      double(viewport_info.xy_extent[0]) * 0.5;
    mi.pixels_per_y = double(viewport_info.ndc_scale[1]) *
                      double(viewport_info.xy_extent[1]) * 0.5;
    mi.read = [this](uint32_t address, uint32_t length) -> const uint8_t* {
      return shared_memory_ ? shared_memory_->StableContents(address, length)
                            : nullptr;
    };
    texture_cache_->MipLodDraw(mi);
  }

  // Trace-only (Bet A shader ceiling, docs/engine-options-20261011.md §9):
  // FABLE2_NS_LOG_CONSTS=<PS hash hex>[,...] logs, for the first FABLE2_NS_LOG_CONSTS_N (default 256)
  // draws of that pixel shader on every pipeline kind (also tessellation/geometry emulation,
  // which FABLE2_NS_LOG_FETCH skips), the system constants, bool/loop constants and all 32
  // fetch constants as hex, for offline specialization. Unset: one getenv.
  {
    // Up to 8 comma-separated hashes; the draw counter is shared.
    static const std::vector<uint64_t> log_list = [] {
      std::vector<uint64_t> list;
      const char* v = std::getenv("FABLE2_NS_LOG_CONSTS");
      while (v && *v && list.size() < 8) {
        char* end = nullptr;
        list.push_back(std::strtoull(v, &end, 16));
        v = (end && *end == ',') ? end + 1 : nullptr;
      }
      return list;
    }();
    const uint64_t log_ps = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    if (!log_list.empty() && pixel_shader &&
        std::find(log_list.begin(), log_list.end(), log_ps) != log_list.end()) {
      static const uint32_t log_n = [] {
        const char* v = std::getenv("FABLE2_NS_LOG_CONSTS_N");
        return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 256u;
      }();
      static std::atomic<uint32_t> logged{0};
      const uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
      if (n < log_n) {
        auto hex = [](const void* p, size_t size) {
          std::string s;
          s.reserve(size * 2);
          const uint8_t* b = static_cast<const uint8_t*>(p);
          static const char* d = "0123456789abcdef";
          for (size_t i = 0; i < size; ++i) {
            s += d[b[i] >> 4];
            s += d[b[i] & 15];
          }
          return s;
        };
        REXLOG_INFO(
            "[ns-consts] ps {:016X} draw {} sys={} bl={} fc={}", log_ps, n,
            hex(&system_constants_, sizeof(system_constants_)),
            hex(&regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                (XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1 -
                 XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) *
                    sizeof(uint32_t)),
            hex(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                32 * 6 * sizeof(uint32_t)));
      }
    }
  }

  float blend_constants[4] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  std::memcpy(dynamic_state_out.blend_constants, blend_constants,
              sizeof(blend_constants));

  dynamic_state_out.stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>();
  dynamic_state_out.stencil_ref_mask_back =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  dynamic_state_out.pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();

  MTL::CullMode cull_mode = MTL::CullModeNone;
  if (primitive_polygonal) {
    bool cull_front = dynamic_state_out.pa_su_sc_mode_cntl.cull_front;
    bool cull_back = dynamic_state_out.pa_su_sc_mode_cntl.cull_back;
    if (cull_front && !cull_back) {
      cull_mode = MTL::CullModeFront;
    } else if (cull_back && !cull_front) {
      cull_mode = MTL::CullModeBack;
    }
  }
  dynamic_state_out.cull_mode = cull_mode;
  dynamic_state_out.front_facing_winding =
      dynamic_state_out.pa_su_sc_mode_cntl.face ? MTL::WindingClockwise
                                                : MTL::WindingCounterClockwise;
  dynamic_state_out.triangle_fill_mode = MTL::TriangleFillModeFill;
  if (primitive_polygonal && dynamic_state_out.pa_su_sc_mode_cntl.poly_mode ==
                                 xenos::PolygonModeEnable::kDualMode) {
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (!dynamic_state_out.pa_su_sc_mode_cntl.cull_front) {
      polygon_type =
          std::min(polygon_type,
                   dynamic_state_out.pa_su_sc_mode_cntl.polymode_front_ptype);
    }
    if (!dynamic_state_out.pa_su_sc_mode_cntl.cull_back) {
      polygon_type =
          std::min(polygon_type,
                   dynamic_state_out.pa_su_sc_mode_cntl.polymode_back_ptype);
    }
    if (polygon_type != xenos::PolygonType::kTriangles) {
      dynamic_state_out.triangle_fill_mode = MTL::TriangleFillModeLines;
    }
  }

  float polygon_offset_scale = 0.0f;
  float polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(
      regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
  dynamic_state_out.depth_bias_constant =
      static_cast<float>(draw_util::GetD3D10IntegerPolygonOffset(
          regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset));
  uint32_t rasterizer_resolution_scale = 1;
  if (render_target_cache_ &&
      !render_target_cache_->IsCurrentPassNativeScale()) {
    rasterizer_resolution_scale =
        std::max(render_target_cache_->draw_resolution_scale_x(),
                 render_target_cache_->draw_resolution_scale_y());
  }
  dynamic_state_out.depth_bias_slope = polygon_offset_scale *
                                       xenos::kPolygonOffsetScaleSubpixelUnit *
                                       float(rasterizer_resolution_scale);
  dynamic_state_out.depth_clip_mode = pa_cl_clip_cntl.clip_disable
                                          ? MTL::DepthClipModeClamp
                                          : MTL::DepthClipModeClip;

  // CBV descriptor table layout:
  //   b0: System constants
  //   b1: Packed float constants
  //   b2: Bool/loop constants
  //   b3: Fetch constants
  //   b4: Descriptor indices
  constexpr size_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
  const size_t kFetchConstantCount =
      xenos::kTextureFetchConstantCount * 6;  // 192 DWORDs = 768 bytes

  const MetalShader::DrawConstantMetadata& vertex_draw_metadata =
      metal_vertex_shader->GetDrawConstantMetadata();
  const MetalShader::DrawConstantMetadata* pixel_draw_metadata =
      metal_pixel_shader ? &metal_pixel_shader->GetDrawConstantMetadata()
                         : nullptr;
  std::array<uint32_t, kStageCount> active_cbv_masks = {
      vertex_draw_metadata.active_cbv_mask,
      pixel_draw_metadata ? pixel_draw_metadata->active_cbv_mask : 0};
  std::array<DxbcShader::FetchConstantDwordMask, kStageCount>
      fetch_constant_dword_masks = {
          metal_vertex_shader
              ? metal_vertex_shader->GetFetchConstantDwordMaskAfterTranslation()
              : DxbcShader::FetchConstantDwordMask(),
          metal_pixel_shader
              ? metal_pixel_shader->GetFetchConstantDwordMaskAfterTranslation()
              : DxbcShader::FetchConstantDwordMask()};
  MergeFetchConstantDwordMask(
      fetch_constant_dword_masks[kStageVertex],
      vertex_draw_metadata.shader_fetch_constant_dword_mask);
  if (pixel_draw_metadata) {
    MergeFetchConstantDwordMask(
        fetch_constant_dword_masks[kStagePixel],
        pixel_draw_metadata->shader_fetch_constant_dword_mask);
  }
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    if ((active_cbv_masks[stage] & (uint32_t(1) << kCbvSlotFetch)) &&
        FetchConstantDwordMaskEmpty(fetch_constant_dword_masks[stage])) {
      MarkAllFetchConstantDwords(fetch_constant_dword_masks[stage]);
    }
  }

  // ---------------------------------------------------------------
  // Allocate dirty constant buffers individually from the upload buffer pool.
  // Clean CBVs keep their previous upload allocation and are re-referenced from
  // the new per-draw descriptor table.
  // ---------------------------------------------------------------

  // Check if float constant layout changed (different shader bound).
  // Matches D3D12 d3d12_command_processor.cc:4910-4943.
  {
    const Shader::ConstantRegisterMap& float_map_vs =
        vertex_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_vertex_[i] !=
          float_map_vs.float_bitmap[i]) {
        current_float_constant_map_vertex_[i] = float_map_vs.float_bitmap[i];
        if (float_map_vs.float_count) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
    if (pixel_shader) {
      const Shader::ConstantRegisterMap& float_map_ps =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        if (current_float_constant_map_pixel_[i] !=
            float_map_ps.float_bitmap[i]) {
          current_float_constant_map_pixel_[i] = float_map_ps.float_bitmap[i];
          if (float_map_ps.float_count) {
            cbuffer_binding_float_pixel_.up_to_date = false;
          }
        }
      }
    } else {
      std::memset(current_float_constant_map_pixel_, 0,
                  sizeof(current_float_constant_map_pixel_));
    }
  }

  const auto& texture_bindings_vertex =
      metal_vertex_shader->GetTextureBindingsAfterTranslation();
  const auto& sampler_bindings_vertex =
      metal_vertex_shader->GetSamplerBindingsAfterTranslation();
  const size_t texture_count_vertex = texture_bindings_vertex.size();
  const size_t sampler_count_vertex = sampler_bindings_vertex.size();
  size_t texture_layout_uid_vertex =
      metal_vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex =
      metal_vertex_shader->GetSamplerBindingLayoutUserUID();
  auto& next_texture_bindless_indices_vertex =
      scratch_texture_bindless_indices_vertex_;
  auto& next_texture_bindless_resources_vertex =
      scratch_texture_bindless_resources_vertex_;
  auto& next_sampler_bindless_indices_vertex =
      scratch_sampler_bindless_indices_vertex_;
  next_texture_bindless_indices_vertex.clear();
  next_texture_bindless_resources_vertex.clear();
  next_sampler_bindless_indices_vertex.clear();
  scratch_descriptor_indices_vertex_.clear();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      auto parameters =
          texture_cache_->GetSamplerParameters(sampler_bindings_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        current_samplers_vertex_[i] = parameters;
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      }
    }
  } else if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
    current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
    cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
  }
  bool vertex_texture_layout_changed =
      current_texture_layout_uid_vertex_ != texture_layout_uid_vertex;
  if (vertex_texture_layout_changed && !texture_count_vertex) {
    cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
  } else if (texture_count_vertex &&
             cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
    bool vertex_texture_srv_changed =
        !vertex_texture_layout_changed &&
        !texture_cache_->AreActiveTextureSRVKeysUpToDate(
            current_texture_srv_keys_vertex_.data(),
            texture_bindings_vertex.data(), texture_count_vertex);
    if (vertex_texture_layout_changed || vertex_texture_srv_changed) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
  }
  if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
    next_texture_bindless_indices_vertex.reserve(texture_count_vertex);
    next_texture_bindless_resources_vertex.reserve(texture_count_vertex);
    for (const auto& binding : texture_bindings_vertex) {
      MTL::Texture* texture_for_encoder = nullptr;
      next_texture_bindless_indices_vertex.push_back(
          texture_cache_->GetBindlessSRVIndexForBinding(
              binding.fetch_constant, binding.dimension, binding.is_signed,
              &texture_for_encoder));
      next_texture_bindless_resources_vertex.push_back(texture_for_encoder);
    }
    next_sampler_bindless_indices_vertex.reserve(sampler_count_vertex);
    // [draw-prep-lite] current_samplers_vertex_ holds this draw's parameters
    // (built above from the same registers).
    const bool sampler_params_ready = DrawPrepLiteEnabled();
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      next_sampler_bindless_indices_vertex.push_back(
          sampler_params_ready
              ? texture_cache_->GetBindlessSamplerIndexForParameters(
                    current_samplers_vertex_[i])
              : texture_cache_->GetBindlessSamplerIndexForBinding(
                    sampler_bindings_vertex[i]));
    }
  }

  size_t texture_layout_uid_pixel = 0;
  size_t sampler_layout_uid_pixel = 0;
  const std::vector<DxbcShader::TextureBinding>* texture_bindings_pixel_ptr =
      nullptr;
  auto& next_texture_bindless_indices_pixel =
      scratch_texture_bindless_indices_pixel_;
  auto& next_texture_bindless_resources_pixel =
      scratch_texture_bindless_resources_pixel_;
  auto& next_sampler_bindless_indices_pixel =
      scratch_sampler_bindless_indices_pixel_;
  next_texture_bindless_indices_pixel.clear();
  next_texture_bindless_resources_pixel.clear();
  next_sampler_bindless_indices_pixel.clear();
  scratch_descriptor_indices_pixel_.clear();
  if (metal_pixel_shader) {
    const auto& texture_bindings_pixel =
        metal_pixel_shader->GetTextureBindingsAfterTranslation();
    const auto& sampler_bindings_pixel =
        metal_pixel_shader->GetSamplerBindingsAfterTranslation();
    const size_t texture_count_pixel = texture_bindings_pixel.size();
    const size_t sampler_count_pixel = sampler_bindings_pixel.size();
    texture_bindings_pixel_ptr = &texture_bindings_pixel;
    texture_layout_uid_pixel =
        metal_pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel =
        metal_pixel_shader->GetSamplerBindingLayoutUserUID();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), sampler_count_pixel));
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        auto parameters =
            texture_cache_->GetSamplerParameters(sampler_bindings_pixel[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        }
      }
    } else if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
      current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }
    bool pixel_texture_layout_changed =
        current_texture_layout_uid_pixel_ != texture_layout_uid_pixel;
    if (pixel_texture_layout_changed && !texture_count_pixel) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else if (texture_count_pixel &&
               cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      bool pixel_texture_srv_changed =
          !pixel_texture_layout_changed &&
          !texture_cache_->AreActiveTextureSRVKeysUpToDate(
              current_texture_srv_keys_pixel_.data(),
              texture_bindings_pixel.data(), texture_count_pixel,
              texture_cache_->miplod_view_levels_);
      if (pixel_texture_layout_changed || pixel_texture_srv_changed) {
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
      }
    }
    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      next_texture_bindless_indices_pixel.reserve(texture_count_pixel);
      next_texture_bindless_resources_pixel.reserve(texture_count_pixel);
      for (const auto& binding : texture_bindings_pixel) {
        MTL::Texture* texture_for_encoder = nullptr;
        next_texture_bindless_indices_pixel.push_back(
            texture_cache_->GetBindlessSRVIndexForBinding(
                binding.fetch_constant, binding.dimension, binding.is_signed,
                &texture_for_encoder,
                texture_cache_->miplod_view_levels_[binding.fetch_constant &
                                                    31]));
        next_texture_bindless_resources_pixel.push_back(texture_for_encoder);
      }
      next_sampler_bindless_indices_pixel.reserve(sampler_count_pixel);
      // [draw-prep-lite] current_samplers_pixel_ holds this draw's parameters
      // (built above from the same registers).
      const bool sampler_params_ready = DrawPrepLiteEnabled();
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        next_sampler_bindless_indices_pixel.push_back(
            sampler_params_ready
                ? texture_cache_->GetBindlessSamplerIndexForParameters(
                      current_samplers_pixel_[i])
                : texture_cache_->GetBindlessSamplerIndexForBinding(
                      sampler_bindings_pixel[i]));
      }
    }
  }

  bool descriptor_indices_vertex_written = false;
  bool descriptor_indices_pixel_written = false;

  auto upload_binding = [&](ConstantBufferBinding& binding, size_t cbv_slot,
                            size_t size, const char* name,
                            auto&& writer) -> bool {
    constexpr size_t kConstantBufferAlignment = 256;
    size = std::max(size, size_t(16));
    MTL::Buffer* buffer = nullptr;
    size_t offset = 0;
    uint64_t gpu_address = 0;
    uint8_t* data = constant_buffer_pool_->Request(
        frame_current_, size, kConstantBufferAlignment, &buffer, offset,
        gpu_address);
    if (!data) {
      REXLOG_ERROR("IssueDraw: {} constant buffer pool allocation failed", name);
      return false;
    }
    writer(data, size);
    binding.buffer = buffer;
    binding.offset = static_cast<NS::UInteger>(offset);
    binding.gpu_address = gpu_address;
    binding.size = size;
    binding.upload_frame = frame_current_;
    binding.up_to_date = true;
    if (cbv_slot < backend_telemetry_.cbv_uploads.size()) {
      ++backend_telemetry_.cbv_uploads[cbv_slot];
    }
    return true;
  };
  // Pack-then-compare wrapper around upload_binding.  Honours the existing
  // "register write dirties live constant range" semantic (the caller still
  // gates on binding.up_to_date == false), but if the freshly packed payload
  // is byte-identical to the previous upload we reuse the prior pool slice
  // instead of allocating a new one.  See the descriptor-indices precedent
  // below for the same pattern applied to the descriptor-indices CBV.
  auto try_reuse_or_upload =
      [&](ConstantBufferBinding& binding, size_t cbv_slot, size_t stage_index,
          std::vector<uint8_t>& current_payload,
          std::vector<uint8_t>& scratch_payload, size_t logical_size,
          const char* name, auto&& writer) -> bool {
    const size_t aligned_size = std::max(logical_size, size_t(16));
    if (lite2 && logical_size >= 16) {
      // [draw-prep-lite2] every writer fills all logical_size bytes (the float
      // writer clears its whole buffer first), so the zero fill is redundant.
      scratch_payload.resize(aligned_size);
    } else {
      scratch_payload.assign(aligned_size, 0);
    }
    writer(scratch_payload.data(), aligned_size);
    // Reuse the prior slice only if it was allocated in this frame. Cross-frame
    // reuse is unsafe because frame-open reclamation may recycle older slices.
    if (binding.buffer && binding.upload_frame == frame_current_ &&
        current_payload.size() == aligned_size &&
        std::memcmp(current_payload.data(), scratch_payload.data(),
                    aligned_size) == 0) {
      binding.up_to_date = true;
      if (cbv_slot < backend_telemetry_.cbv_reuse_hits.size()) {
        ++backend_telemetry_.cbv_reuse_hits[cbv_slot];
      }
      return true;
    }
    if (TryReuseConstantPayloadFromCache(binding, cbv_slot, stage_index,
                                         scratch_payload.data(),
                                         scratch_payload.size())) {
      if (lite2) {
        // [draw-prep-lite2] the scratch is refilled before its next read.
        current_payload.swap(scratch_payload);
      } else {
        current_payload = scratch_payload;
      }
      return true;
    }
    if (!upload_binding(binding, cbv_slot, logical_size, name,
                        [&](uint8_t* data, size_t out_size) {
                          std::memcpy(
                              data, scratch_payload.data(),
                              std::min(out_size, scratch_payload.size()));
                        })) {
      return false;
    }
    StoreConstantPayloadInCache(binding, cbv_slot, stage_index,
                                scratch_payload.data(), scratch_payload.size());
    current_payload.swap(scratch_payload);
    return true;
  };

  if (!cbuffer_binding_system_.up_to_date) {
    if (!try_reuse_or_upload(
            cbuffer_binding_system_, kCbvSlotSystem,
            kConstantPayloadSharedStage, current_payload_system_,
            scratch_payload_system_,
            sizeof(DxbcShaderTranslator::SystemConstants), "system",
            [&](uint8_t* data, size_t) {
              std::memcpy(data, &system_constants_,
                          sizeof(DxbcShaderTranslator::SystemConstants));
            })) {
      return false;
    }
  } else {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotSystem];
  }

  auto write_packed_float_constants =
      [&](uint8_t* dst, size_t dst_size, const Shader::ConstantRegisterMap* map,
          uint32_t regs_base) {
        std::memset(dst, 0, dst_size);
        if (!map || !map->float_count) {
          return;
        }
        uint8_t* out = dst;
        uint8_t* end = dst + dst_size;
        if (lite2) {
          // [draw-prep-lite2] the same bytes in the same order, one memcpy per
          // run of consecutive used constants (inline ctz, no cross-library
          // bit_scan_forward call per constant).
          for (uint32_t i = 0; i < 4; ++i) {
            uint64_t bits = map->float_bitmap[i];
            while (bits) {
              const uint32_t first = uint32_t(__builtin_ctzll(bits));
              const uint64_t from_first = bits >> first;
              const uint32_t run =
                  ~from_first ? uint32_t(__builtin_ctzll(~from_first))
                              : 64 - first;
              const size_t room = size_t(end - out) / (4 * sizeof(uint32_t));
              const uint32_t copy = uint32_t(std::min<size_t>(run, room));
              std::memcpy(out,
                          &regs.values[regs_base + (i << 8) + (first << 2)],
                          size_t(copy) * 4 * sizeof(uint32_t));
              out += size_t(copy) * 4 * sizeof(uint32_t);
              if (copy < run) {
                return;
              }
              bits = (first + run >= 64) ? 0 : (bits & (~uint64_t(0) << (first + run)));
            }
          }
          return;
        }
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t bits = map->float_bitmap[i];
          uint32_t constant_index;
          while (rex::bit_scan_forward(bits, &constant_index)) {
            bits &= ~(uint64_t(1) << constant_index);
            if (out + 4 * sizeof(uint32_t) > end) {
              return;
            }
            std::memcpy(
                out, &regs.values[regs_base + (i << 8) + (constant_index << 2)],
                4 * sizeof(uint32_t));
            out += 4 * sizeof(uint32_t);
          }
        }
      };

  const Shader::ConstantRegisterMap& float_map_vertex =
      vertex_shader->constant_register_map();
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    const size_t float_size =
        sizeof(float) * 4 * std::max(float_map_vertex.float_count, uint32_t(1));
    if (!try_reuse_or_upload(
            cbuffer_binding_float_vertex_, kCbvSlotFloat, kStageVertex,
            current_payload_float_vertex_, scratch_payload_float_vertex_,
            float_size, "vertex float", [&](uint8_t* data, size_t size) {
              write_packed_float_constants(data, size, &float_map_vertex,
                                           XE_GPU_REG_SHADER_CONSTANT_000_X);
            })) {
      return false;
    }
  } else {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFloat];
  }

  if (!cbuffer_binding_float_pixel_.up_to_date) {
    const Shader::ConstantRegisterMap* float_map_pixel =
        pixel_shader ? &pixel_shader->constant_register_map() : nullptr;
    const size_t float_size =
        sizeof(float) * 4 *
        std::max(float_map_pixel ? float_map_pixel->float_count : uint32_t(0),
                 uint32_t(1));
    if (!try_reuse_or_upload(
            cbuffer_binding_float_pixel_, kCbvSlotFloat, kStagePixel,
            current_payload_float_pixel_, scratch_payload_float_pixel_,
            float_size, "pixel float", [&](uint8_t* data, size_t size) {
              write_packed_float_constants(data, size, float_map_pixel,
                                           XE_GPU_REG_SHADER_CONSTANT_256_X);
            })) {
      return false;
    }
  } else if (pixel_shader) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFloat];
  }

  if (!cbuffer_binding_bool_loop_.up_to_date) {
    if (!try_reuse_or_upload(
            cbuffer_binding_bool_loop_, kCbvSlotBoolLoop,
            kConstantPayloadSharedStage, current_payload_bool_loop_,
            scratch_payload_bool_loop_, kBoolLoopConstantsSize, "bool loop",
            [&](uint8_t* data, size_t) {
              std::memcpy(data,
                          &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                          kBoolLoopConstantsSize);
            })) {
      return false;
    }
  } else {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotBoolLoop];
  }

  std::array<bool, kStageCount> fetch_binding_active = {};
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    if (!(active_cbv_masks[stage] & (uint32_t(1) << kCbvSlotFetch))) {
      continue;
    }
    fetch_binding_active[stage] = true;
    if (FetchConstantDwordMasksOverlap(fetch_constant_dirty_masks_[stage],
                                       fetch_constant_dword_masks[stage])) {
      cbuffer_binding_fetch_[stage].up_to_date = false;
    }
  }
  const size_t fetch_size = kFetchConstantCount * sizeof(uint32_t);
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    if (!fetch_binding_active[stage]) {
      continue;
    }
    if (!cbuffer_binding_fetch_[stage].up_to_date) {
      if (!try_reuse_or_upload(
              cbuffer_binding_fetch_[stage], kCbvSlotFetch,
              kConstantPayloadSharedStage, current_payload_fetch_[stage],
              scratch_payload_fetch_[stage], fetch_size, "fetch",
              [&](uint8_t* data, size_t) {
                std::memcpy(data,
                            &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                            fetch_size);
              })) {
        return false;
      }
      fetch_constant_dirty_masks_[stage].fill(0);
    } else {
      ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFetch];
    }
  }

  auto build_descriptor_indices =
      [&](MetalShader* shader, std::vector<uint32_t>& words,
          const MetalShader::DrawConstantMetadata* metadata,
          const std::vector<uint32_t>& texture_indices,
          const std::vector<uint32_t>& sampler_indices) {
        words.assign(metadata ? metadata->descriptor_indices_word_count : 1, 0);
        if (!shader || !texture_cache_) {
          return;
        }
        const auto& tex_bindings = shader->GetTextureBindingsAfterTranslation();
        for (size_t i = 0;
             i < tex_bindings.size() && i < texture_indices.size(); ++i) {
          uint32_t d = tex_bindings[i].bindless_descriptor_index;
          assert_true(d < words.size());
          if (d >= words.size()) {
            continue;
          }
          words[d] = texture_indices[i];
        }
        const auto& smp_bindings = shader->GetSamplerBindingsAfterTranslation();
        for (size_t i = 0;
             i < smp_bindings.size() && i < sampler_indices.size(); ++i) {
          uint32_t d = smp_bindings[i].bindless_descriptor_index;
          assert_true(d < words.size());
          if (d >= words.size()) {
            continue;
          }
          words[d] = sampler_indices[i];
        }
      };

  auto upload_descriptor_indices = [&](ConstantBufferBinding& binding,
                                       const std::vector<uint32_t>& words,
                                       size_t stage, const char* name) -> bool {
    const size_t descriptor_indices_bytes = words.size() * sizeof(uint32_t);
    const size_t descriptor_payload_size =
        std::max(descriptor_indices_bytes, size_t(16));
    // [draw-prep-lite2] reused storage instead of a vector per upload.
    std::vector<uint8_t> descriptor_payload_local;
    std::vector<uint8_t>& descriptor_payload =
        lite2 ? descriptor_payload_scratch_ : descriptor_payload_local;
    descriptor_payload.assign(descriptor_payload_size, 0);
    if (descriptor_indices_bytes) {
      std::memcpy(descriptor_payload.data(), words.data(),
                  descriptor_indices_bytes);
    }
    if (TryReuseConstantPayloadFromCache(binding, kCbvSlotDescriptorIndices,
                                         stage, descriptor_payload.data(),
                                         descriptor_payload.size())) {
      return true;
    }
    if (stage < backend_telemetry_.descriptor_index_uploads.size()) {
      ++backend_telemetry_.descriptor_index_uploads[stage];
    }
    if (!upload_binding(
            binding, kCbvSlotDescriptorIndices, descriptor_indices_bytes, name,
            [&](uint8_t* data, size_t size) {
              std::memcpy(data, descriptor_payload.data(),
                          std::min(size, descriptor_payload.size()));
            })) {
      return false;
    }
    StoreConstantPayloadInCache(binding, kCbvSlotDescriptorIndices, stage,
                                descriptor_payload.data(),
                                descriptor_payload.size());
    return true;
  };

  if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
    build_descriptor_indices(
        metal_vertex_shader, scratch_descriptor_indices_vertex_,
        &vertex_draw_metadata, next_texture_bindless_indices_vertex,
        next_sampler_bindless_indices_vertex);
    const bool can_reuse_descriptor_indices =
        cbuffer_binding_descriptor_indices_vertex_.buffer &&
        cbuffer_binding_descriptor_indices_vertex_.upload_frame ==
            frame_current_ &&
        current_descriptor_indices_vertex_ ==
            scratch_descriptor_indices_vertex_;
    if (can_reuse_descriptor_indices) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      ++backend_telemetry_.cbv_reuse_hits[kCbvSlotDescriptorIndices];
    } else {
      if (!upload_descriptor_indices(cbuffer_binding_descriptor_indices_vertex_,
                                     scratch_descriptor_indices_vertex_,
                                     kStageVertex,
                                     "vertex descriptor indices")) {
        return false;
      }
      current_descriptor_indices_vertex_.swap(
          scratch_descriptor_indices_vertex_);
    }
    descriptor_indices_vertex_written = true;
  } else {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotDescriptorIndices];
  }

  if (metal_pixel_shader &&
      !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
    build_descriptor_indices(
        metal_pixel_shader, scratch_descriptor_indices_pixel_,
        pixel_draw_metadata, next_texture_bindless_indices_pixel,
        next_sampler_bindless_indices_pixel);
    const bool can_reuse_descriptor_indices =
        cbuffer_binding_descriptor_indices_pixel_.buffer &&
        cbuffer_binding_descriptor_indices_pixel_.upload_frame ==
            frame_current_ &&
        current_descriptor_indices_pixel_ == scratch_descriptor_indices_pixel_;
    if (can_reuse_descriptor_indices) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      ++backend_telemetry_.cbv_reuse_hits[kCbvSlotDescriptorIndices];
    } else {
      if (!upload_descriptor_indices(cbuffer_binding_descriptor_indices_pixel_,
                                     scratch_descriptor_indices_pixel_,
                                     kStagePixel, "pixel descriptor indices")) {
        return false;
      }
      current_descriptor_indices_pixel_.swap(scratch_descriptor_indices_pixel_);
    }
    descriptor_indices_pixel_written = true;
  } else if (metal_pixel_shader) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotDescriptorIndices];
  }

  if (descriptor_indices_vertex_written) {
    current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
    current_texture_bindless_resources_vertex_.swap(
        next_texture_bindless_resources_vertex);
    if (texture_count_vertex) {
      current_texture_srv_keys_vertex_.resize(std::max(
          current_texture_srv_keys_vertex_.size(), texture_count_vertex));
      texture_cache_->WriteActiveTextureSRVKeys(
          current_texture_srv_keys_vertex_.data(),
          texture_bindings_vertex.data(), texture_count_vertex);
    }
  }
  if (descriptor_indices_pixel_written) {
    current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
    current_texture_bindless_resources_pixel_.swap(
        next_texture_bindless_resources_pixel);
    if (texture_bindings_pixel_ptr && !texture_bindings_pixel_ptr->empty()) {
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(),
                   texture_bindings_pixel_ptr->size()));
      texture_cache_->WriteActiveTextureSRVKeys(
          current_texture_srv_keys_pixel_.data(),
          texture_bindings_pixel_ptr->data(),
          texture_bindings_pixel_ptr->size(),
          texture_cache_->miplod_view_levels_);
    }
  }
  if (descriptor_indices_vertex_written || descriptor_indices_pixel_written) {
    PublishBindlessTextureResourceSet();
  }

  uniforms_out = {};
  auto set_uniform_cbv = [&](UniformBufferInfo::Cbv& cbv,
                             const ConstantBufferBinding& binding,
                             bool active) {
    cbv.active = active;
    if (!active) {
      return;
    }
    assert_true(binding.up_to_date);
    assert_true(binding.upload_frame == frame_current_);
    cbv.buffer = binding.buffer;
    cbv.offset = binding.offset;
    cbv.gpu_address = binding.gpu_address;
    cbv.size = binding.size;
  };
  uniforms_out.active_cbv_masks = active_cbv_masks;
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotSystem],
                  cbuffer_binding_system_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotSystem));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotFloat],
                  cbuffer_binding_float_vertex_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotFloat));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotBoolLoop],
                  cbuffer_binding_bool_loop_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotBoolLoop));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotFetch],
                  cbuffer_binding_fetch_[kStageVertex],
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotFetch));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotDescriptorIndices],
                  cbuffer_binding_descriptor_indices_vertex_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotDescriptorIndices));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotSystem],
                  cbuffer_binding_system_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotSystem));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotFloat],
                  cbuffer_binding_float_pixel_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotFloat));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotBoolLoop],
                  cbuffer_binding_bool_loop_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotBoolLoop));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotFetch],
                  cbuffer_binding_fetch_[kStagePixel],
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotFetch));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotDescriptorIndices],
                  cbuffer_binding_descriptor_indices_pixel_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotDescriptorIndices));
  uniforms_out.fetch_constant_dword_masks = fetch_constant_dword_masks;
  current_fetch_constant_dword_masks_ = uniforms_out.fetch_constant_dword_masks;
  return true;
}

void MetalCommandProcessor::ApplyDrawDynamicState(
    const DrawDynamicState& dynamic_state) {
  if (viewport_dirty_ || std::memcmp(&dynamic_state.viewport, &cached_viewport_,
                                     sizeof(MTL::Viewport)) != 0) {
    current_render_encoder_->setViewport(dynamic_state.viewport);
    cached_viewport_ = dynamic_state.viewport;
    viewport_dirty_ = false;
  }

  if (scissor_dirty_ || std::memcmp(&dynamic_state.scissor, &cached_scissor_,
                                    sizeof(MTL::ScissorRect)) != 0) {
    current_render_encoder_->setScissorRect(dynamic_state.scissor);
    cached_scissor_ = dynamic_state.scissor;
    scissor_dirty_ = false;
  }

  if (dynamic_state.rasterization_enabled) {
    ApplyRasterizerState(dynamic_state);
  }

  // Fixed-function depth/stencil state is not part of the pipeline state in
  // Metal, so update it per draw.
  ApplyDepthStencilState(dynamic_state);

  bool blend_factor_update_needed =
      !ff_blend_factor_valid_ ||
      std::memcmp(ff_blend_factor_, dynamic_state.blend_constants,
                  sizeof(float) * 4) != 0;
  if (blend_factor_update_needed) {
    std::memcpy(ff_blend_factor_, dynamic_state.blend_constants,
                sizeof(float) * 4);
    ff_blend_factor_valid_ = true;
    current_render_encoder_->setBlendColor(
        dynamic_state.blend_constants[0], dynamic_state.blend_constants[1],
        dynamic_state.blend_constants[2], dynamic_state.blend_constants[3]);
  }
}

MetalCommandProcessor::StageRootArgumentEntries
MetalCommandProcessor::BuildGraphicsRootArgumentEntries(
    const UniformBufferInfo& uniforms, bool shared_memory_is_uav,
    bool use_tessellation_emulation) const {
  StageRootArgumentEntries entries = {};
  constexpr uint64_t kDescriptorEntrySize = sizeof(IRDescriptorTableEntry);
  uint64_t view_heap_gpu = view_bindless_heap_->gpuAddress();
  uint64_t sampler_heap_gpu = sampler_bindless_heap_->gpuAddress();
  uint64_t system_view_gpu = system_view_tables_->gpuAddress();
  uint64_t srv_space0_gpu =
      system_view_gpu + (shared_memory_is_uav
                             ? kSystemViewTableSRVNull
                             : kSystemViewTableSRVSharedMemory) *
                            kDescriptorEntrySize;
  uint64_t uav_space0_gpu =
      system_view_gpu + (shared_memory_is_uav
                             ? kSystemViewTableUAVSharedMemoryStart
                             : kSystemViewTableUAVNullStart) *
                            kDescriptorEntrySize;
  uint64_t null_uav_gpu =
      system_view_gpu + kSystemViewTableUAVNullStart * kDescriptorEntrySize;
  const uint64_t null_gpu = null_buffer_->gpuAddress();

  entries[kGraphicsRootABSlotSRVSpace0] = srv_space0_gpu;
  entries[kGraphicsRootABSlotSRVSpace1] = view_heap_gpu;
  entries[kGraphicsRootABSlotSRVSpace2] = view_heap_gpu;
  entries[kGraphicsRootABSlotSRVSpace3] = view_heap_gpu;
  entries[kGraphicsRootABSlotSRVSpace10] = view_heap_gpu;
  entries[kGraphicsRootABSlotUAVSpace0] = uav_space0_gpu;
  entries[kGraphicsRootABSlotUAVSpace1] = null_uav_gpu;
  entries[kGraphicsRootABSlotUAVSpace2] = null_uav_gpu;
  entries[kGraphicsRootABSlotUAVSpace3] = null_uav_gpu;
  entries[kGraphicsRootABSlotSamplerSpace0] = sampler_heap_gpu;

  auto cbv_address = [&](size_t stage, size_t cbv) -> uint64_t {
    const uint32_t cbv_bit = uint32_t(1) << cbv;
    if (!(uniforms.active_cbv_masks[stage] & cbv_bit)) {
      return null_gpu;
    }
    const UniformBufferInfo::Cbv& uniform_cbv = uniforms.cbvs[stage][cbv];
    return uniform_cbv.gpu_address ? uniform_cbv.gpu_address : null_gpu;
  };
  auto common_cbv_address = [&](size_t cbv) -> uint64_t {
    uint64_t vertex_address = cbv_address(kStageVertex, cbv);
    if (vertex_address != null_gpu) {
      return vertex_address;
    }
    return cbv_address(kStagePixel, cbv);
  };
  auto set_stage_local_cbvs = [&](uint32_t float_slot, uint32_t fetch_slot,
                                  uint32_t descriptor_slot, size_t stage) {
    entries[float_slot] = cbv_address(stage, kCbvSlotFloat);
    entries[fetch_slot] = cbv_address(stage, kCbvSlotFetch);
    entries[descriptor_slot] = cbv_address(stage, kCbvSlotDescriptorIndices);
  };

  entries[kGraphicsRootABSlotCBVSystem] = common_cbv_address(kCbvSlotSystem);
  entries[kGraphicsRootABSlotCBVBoolLoop] =
      common_cbv_address(kCbvSlotBoolLoop);
  set_stage_local_cbvs(
      kGraphicsRootABSlotCBVVertexFloat, kGraphicsRootABSlotCBVVertexFetch,
      kGraphicsRootABSlotCBVVertexDescriptorIndices, kStageVertex);
  if (use_tessellation_emulation) {
    // Hull and domain generated stages consume the same guest constant state as
    // the vertex/domain translation they expand. Keep these slots null on
    // ordinary draws so normal vertex root churn doesn't patch inactive stages.
    set_stage_local_cbvs(
        kGraphicsRootABSlotCBVHullFloat, kGraphicsRootABSlotCBVHullFetch,
        kGraphicsRootABSlotCBVHullDescriptorIndices, kStageVertex);
    set_stage_local_cbvs(
        kGraphicsRootABSlotCBVDomainFloat, kGraphicsRootABSlotCBVDomainFetch,
        kGraphicsRootABSlotCBVDomainDescriptorIndices, kStageVertex);
  } else {
    entries[kGraphicsRootABSlotCBVHullFloat] = null_gpu;
    entries[kGraphicsRootABSlotCBVHullFetch] = null_gpu;
    entries[kGraphicsRootABSlotCBVHullDescriptorIndices] = null_gpu;
    entries[kGraphicsRootABSlotCBVDomainFloat] = null_gpu;
    entries[kGraphicsRootABSlotCBVDomainFetch] = null_gpu;
    entries[kGraphicsRootABSlotCBVDomainDescriptorIndices] = null_gpu;
  }
  set_stage_local_cbvs(
      kGraphicsRootABSlotCBVPixelFloat, kGraphicsRootABSlotCBVPixelFetch,
      kGraphicsRootABSlotCBVPixelDescriptorIndices, kStagePixel);
  return entries;
}

bool MetalCommandProcessor::AllocateStageRootArgument(
    size_t /* stage_index */, const StageRootArgumentEntries& entries,
    StageRootArgumentAllocation& allocation_out) {
  MTL::Buffer* top_level_buffer = nullptr;
  size_t top_level_offset = 0;
  uint64_t top_level_gpu_address = 0;
  auto* top_level_entries =
      reinterpret_cast<uint64_t*>(constant_buffer_pool_->Request(
          frame_current_, kTopLevelABBytesPerTable, kTopLevelABBytesPerTable,
          &top_level_buffer, top_level_offset, top_level_gpu_address));
  if (!top_level_entries) {
    REXLOG_ERROR("IssueDraw: bindless table allocation failed");
    return false;
  }

  std::memcpy(top_level_entries, entries.data(), kTopLevelABBytesPerTable);

  allocation_out = {top_level_buffer,
                    static_cast<NS::UInteger>(top_level_offset),
                    top_level_gpu_address, frame_current_, true};
  return true;
}

bool MetalCommandProcessor::TryReuseConstantPayloadFromCache(
    ConstantBufferBinding& binding, size_t cbv_slot, size_t stage_index,
    const uint8_t* payload, size_t size) {
  if (!REXCVAR_GET(metal_constant_payload_cache)) {
    return false;
  }
  if (!payload || !size) {
    return false;
  }
  const uint64_t hash = XXH3_64bits(payload, size);
  auto it = constant_payload_cache_.find(hash);
  if (it != constant_payload_cache_.end()) {
    for (const ConstantPayloadCacheEntry& entry : it->second) {
      if (entry.cbv_slot != cbv_slot || entry.stage_index != stage_index ||
          entry.size != size || entry.binding.upload_frame != frame_current_) {
        continue;
      }
      if (entry.bytes.size() != size ||
          std::memcmp(entry.bytes.data(), payload, size) != 0) {
        continue;
      }
      binding = entry.binding;
      binding.up_to_date = true;
      if (cbv_slot < backend_telemetry_.constant_payload_cache_hits.size()) {
        ++backend_telemetry_.constant_payload_cache_hits[cbv_slot];
        backend_telemetry_.constant_payload_cache_bytes_saved[cbv_slot] += size;
        ++backend_telemetry_.cbv_reuse_hits[cbv_slot];
      }
      if (cbv_slot == kCbvSlotDescriptorIndices && stage_index < kStageCount) {
        ++backend_telemetry_.descriptor_index_payload_cache_hits[stage_index];
        backend_telemetry_
            .descriptor_index_payload_cache_bytes_saved[stage_index] += size;
      }
      return true;
    }
  }
  if (cbv_slot < backend_telemetry_.constant_payload_cache_misses.size()) {
    ++backend_telemetry_.constant_payload_cache_misses[cbv_slot];
  }
  if (cbv_slot == kCbvSlotDescriptorIndices && stage_index < kStageCount) {
    ++backend_telemetry_.descriptor_index_payload_cache_misses[stage_index];
  }
  return false;
}

void MetalCommandProcessor::StoreConstantPayloadInCache(
    const ConstantBufferBinding& binding, size_t cbv_slot, size_t stage_index,
    const uint8_t* payload, size_t size) {
  if (!REXCVAR_GET(metal_constant_payload_cache)) {
    return;
  }
  if (!binding.buffer || !payload || !size ||
      binding.upload_frame != frame_current_ ||
      constant_payload_cache_bytes_ + size > kConstantPayloadCacheMaxBytes) {
    return;
  }
  const uint64_t hash = XXH3_64bits(payload, size);
  auto& entries = constant_payload_cache_[hash];
  for (const ConstantPayloadCacheEntry& entry : entries) {
    if (entry.cbv_slot == cbv_slot && entry.stage_index == stage_index &&
        entry.size == size && entry.bytes.size() == size &&
        std::memcmp(entry.bytes.data(), payload, size) == 0) {
      return;
    }
  }
  ConstantPayloadCacheEntry entry;
  entry.cbv_slot = cbv_slot;
  entry.stage_index = stage_index;
  entry.size = size;
  entry.binding = binding;
  entry.bytes.assign(payload, payload + size);
  entries.push_back(std::move(entry));
  constant_payload_cache_bytes_ += size;
}

bool MetalCommandProcessor::PopulateBindlessTables(
    bool shared_memory_is_uav, MTL::ResourceUsage shared_memory_usage,
    bool use_geometry_emulation, bool use_tessellation_emulation,
    const UniformBufferInfo& uniforms) {
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    if (current_bindless_stage_root_valid_[stage]) {
      assert_true(current_bindless_stage_root_arguments_[stage].upload_frame ==
                  frame_current_);
    }
  }

  constexpr size_t kGraphicsRootStage = kStageVertex;
  const bool bindless_shared_memory_uav_mismatch =
      current_bindless_shared_memory_is_uav_ != shared_memory_is_uav;
  StageRootArgumentEntries entries = BuildGraphicsRootArgumentEntries(
      uniforms, shared_memory_is_uav, use_tessellation_emulation);
  const bool entries_were_valid =
      current_bindless_stage_root_entries_valid_[kGraphicsRootStage];
  const bool root_rebuild_detail_telemetry =
      REXCVAR_GET(metal_root_rebuild_detail_telemetry);
  const uint64_t null_gpu = root_rebuild_detail_telemetry && null_buffer_
                                ? null_buffer_->gpuAddress()
                                : 0;
  struct RootCbvIdentity {
    bool active = false;
    MTL::Buffer* buffer = nullptr;
    NS::UInteger offset = 0;
  };
  auto root_slot_to_cbv = [&](size_t slot, size_t& stage, size_t& cbv,
                              bool& common) -> bool {
    common = false;
    switch (slot) {
      case kGraphicsRootABSlotCBVSystem:
        common = true;
        cbv = kCbvSlotSystem;
        return true;
      case kGraphicsRootABSlotCBVBoolLoop:
        common = true;
        cbv = kCbvSlotBoolLoop;
        return true;
      case kGraphicsRootABSlotCBVVertexFloat:
      case kGraphicsRootABSlotCBVHullFloat:
      case kGraphicsRootABSlotCBVDomainFloat:
        stage = kStageVertex;
        cbv = kCbvSlotFloat;
        return true;
      case kGraphicsRootABSlotCBVVertexFetch:
      case kGraphicsRootABSlotCBVHullFetch:
      case kGraphicsRootABSlotCBVDomainFetch:
        stage = kStageVertex;
        cbv = kCbvSlotFetch;
        return true;
      case kGraphicsRootABSlotCBVVertexDescriptorIndices:
      case kGraphicsRootABSlotCBVHullDescriptorIndices:
      case kGraphicsRootABSlotCBVDomainDescriptorIndices:
        stage = kStageVertex;
        cbv = kCbvSlotDescriptorIndices;
        return true;
      case kGraphicsRootABSlotCBVPixelFloat:
        stage = kStagePixel;
        cbv = kCbvSlotFloat;
        return true;
      case kGraphicsRootABSlotCBVPixelFetch:
        stage = kStagePixel;
        cbv = kCbvSlotFetch;
        return true;
      case kGraphicsRootABSlotCBVPixelDescriptorIndices:
        stage = kStagePixel;
        cbv = kCbvSlotDescriptorIndices;
        return true;
      default:
        return false;
    }
  };
  auto uniform_cbv_identity = [&](size_t stage, size_t cbv) -> RootCbvIdentity {
    const uint32_t cbv_bit = uint32_t(1) << cbv;
    if (!(uniforms.active_cbv_masks[stage] & cbv_bit)) {
      return {};
    }
    const UniformBufferInfo::Cbv& uniform_cbv = uniforms.cbvs[stage][cbv];
    return {true, uniform_cbv.buffer, uniform_cbv.offset};
  };
  auto current_cbv_identity = [&](size_t stage, size_t cbv) -> RootCbvIdentity {
    const uint32_t cbv_bit = uint32_t(1) << cbv;
    if (!(current_bindless_active_cbv_masks_[stage] & cbv_bit)) {
      return {};
    }
    return {true, current_bindless_cbv_buffers_[stage][cbv],
            current_bindless_cbv_offsets_[stage][cbv]};
  };
  auto select_common_uniform_cbv_identity = [&](size_t cbv) -> RootCbvIdentity {
    RootCbvIdentity vertex_identity = uniform_cbv_identity(kStageVertex, cbv);
    if (vertex_identity.active) {
      return vertex_identity;
    }
    return uniform_cbv_identity(kStagePixel, cbv);
  };
  auto select_common_current_cbv_identity = [&](size_t cbv) -> RootCbvIdentity {
    RootCbvIdentity vertex_identity = current_cbv_identity(kStageVertex, cbv);
    if (vertex_identity.active) {
      return vertex_identity;
    }
    return current_cbv_identity(kStagePixel, cbv);
  };
  size_t slots_patched = 0;
  bool descriptor_indices_pointer_mismatch = false;
  bool other_cbv_pointer_mismatch = false;
  bool root_resource_identity_changed = false;
  bool root_resource_identity_same = false;
  for (size_t slot = 0; slot < entries.size(); ++slot) {
    const bool slot_changed =
        entries_were_valid
            ? current_bindless_stage_root_entries_[kGraphicsRootStage][slot] !=
                  entries[slot]
            : entries[slot] != 0;
    if (!slot_changed) {
      continue;
    }
    ++slots_patched;
    if (slot < backend_telemetry_.bindless_root_arg_slot_patches.size()) {
      ++backend_telemetry_.bindless_root_arg_slot_patches[slot];
    }
    if (root_rebuild_detail_telemetry) {
      const bool previous_slot_active =
          entries_were_valid &&
          current_bindless_stage_root_entries_[kGraphicsRootStage][slot] !=
              null_gpu;
      const bool new_slot_active = entries[slot] != null_gpu;
      size_t cbv_stage = kStageVertex;
      size_t cbv_slot = 0;
      bool common_cbv = false;
      const bool root_slot_is_cbv =
          root_slot_to_cbv(slot, cbv_stage, cbv_slot, common_cbv);
      if (root_slot_is_cbv) {
        RootCbvIdentity previous_identity =
            common_cbv ? select_common_current_cbv_identity(cbv_slot)
                       : current_cbv_identity(cbv_stage, cbv_slot);
        RootCbvIdentity new_identity =
            common_cbv ? select_common_uniform_cbv_identity(cbv_slot)
                       : uniform_cbv_identity(cbv_stage, cbv_slot);
        previous_identity.active &= previous_slot_active;
        new_identity.active &= new_slot_active;
        previous_identity.buffer =
            previous_identity.active ? previous_identity.buffer : nullptr;
        new_identity.buffer =
            new_identity.active ? new_identity.buffer : nullptr;
        if (previous_identity.buffer == new_identity.buffer &&
            previous_identity.buffer &&
            previous_identity.offset != new_identity.offset) {
          ++backend_telemetry_.bindless_root_rebuild_details
                [kBindlessRootDetailSameBufferOffsetChanged];
          root_resource_identity_same = true;
        } else if (previous_identity.buffer != new_identity.buffer) {
          ++backend_telemetry_.bindless_root_rebuild_details
                [kBindlessRootDetailDifferentBuffer];
          root_resource_identity_changed = true;
        } else if (previous_identity.buffer) {
          root_resource_identity_same = true;
        }
      } else {
        root_resource_identity_changed = true;
      }
    }
    if (entries_were_valid) {
      switch (slot) {
        case kGraphicsRootABSlotCBVVertexDescriptorIndices:
        case kGraphicsRootABSlotCBVHullDescriptorIndices:
        case kGraphicsRootABSlotCBVDomainDescriptorIndices:
        case kGraphicsRootABSlotCBVPixelDescriptorIndices:
          descriptor_indices_pointer_mismatch = true;
          break;
        case kGraphicsRootABSlotCBVSystem:
        case kGraphicsRootABSlotCBVVertexFloat:
        case kGraphicsRootABSlotCBVBoolLoop:
        case kGraphicsRootABSlotCBVVertexFetch:
        case kGraphicsRootABSlotCBVHullFloat:
        case kGraphicsRootABSlotCBVHullFetch:
        case kGraphicsRootABSlotCBVDomainFloat:
        case kGraphicsRootABSlotCBVDomainFetch:
        case kGraphicsRootABSlotCBVPixelFloat:
        case kGraphicsRootABSlotCBVPixelFetch:
          other_cbv_pointer_mismatch = true;
          break;
        default:
          break;
      }
    }
  }

  const bool root_valid =
      current_bindless_stage_root_valid_[kGraphicsRootStage] &&
      current_bindless_stage_root_arguments_[kGraphicsRootStage].valid;
  const bool root_update_needed = !root_valid || !entries_were_valid ||
                                  slots_patched ||
                                  bindless_shared_memory_uav_mismatch;
  if (root_update_needed) {
    if (root_rebuild_detail_telemetry) {
      const size_t slots_changed_bin =
          std::min(slots_patched,
                   backend_telemetry_.bindless_root_slots_changed.size() - 1);
      ++backend_telemetry_.bindless_root_slots_changed[slots_changed_bin];
      if (descriptor_indices_pointer_mismatch && !other_cbv_pointer_mismatch) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailDescriptorIndicesOnly];
      } else if (!descriptor_indices_pointer_mismatch &&
                 other_cbv_pointer_mismatch) {
        ++backend_telemetry_
              .bindless_root_rebuild_details[kBindlessRootDetailOtherCbvOnly];
      } else if (descriptor_indices_pointer_mismatch &&
                 other_cbv_pointer_mismatch) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailMixedDescriptorAndOther];
      }
      if (root_resource_identity_changed) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailResourceIdentityChanged];
      } else if (root_resource_identity_same || slots_patched) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailResourceIdentitySame];
      }
    }
    const bool frame_open_rebuild =
        current_bindless_stage_root_frame_open_rebuild_pending_
            [kGraphicsRootStage];
    if (frame_open_rebuild) {
      ++backend_telemetry_
            .bindless_root_rebuild_reasons[kBindlessRootRebuildFrameOpen];
    }
    if (!frame_open_rebuild && descriptor_indices_pointer_mismatch) {
      ++backend_telemetry_.bindless_root_rebuild_reasons
            [kBindlessRootRebuildDescriptorIndicesPointerChange];
    }
    if (!frame_open_rebuild && other_cbv_pointer_mismatch) {
      ++backend_telemetry_.bindless_root_rebuild_reasons
            [kBindlessRootRebuildOtherCbvPointerChange];
    }
    if (bindless_shared_memory_uav_mismatch) {
      ++backend_telemetry_.bindless_root_rebuild_reasons
            [kBindlessRootRebuildSharedMemoryUavChange];
    }
    if (entries_were_valid && !slots_patched && root_valid) {
      ++backend_telemetry_.bindless_root_arg_noop_updates[kGraphicsRootStage];
    } else {
      StageRootArgumentAllocation allocation;
      if (!AllocateStageRootArgument(kGraphicsRootStage, entries, allocation)) {
        return false;
      }
      current_bindless_stage_root_arguments_[kGraphicsRootStage] = allocation;
      ++backend_telemetry_.bindless_root_allocations[kGraphicsRootStage];
      backend_telemetry_.bindless_root_arg_slots_patched[kGraphicsRootStage] +=
          slots_patched;
      backend_telemetry_.bindless_root_arg_bytes_copied[kGraphicsRootStage] +=
          kTopLevelABBytesPerTable;
      ++current_bindless_stage_root_serials_[kGraphicsRootStage];
      if (!current_bindless_stage_root_serials_[kGraphicsRootStage]) {
        current_bindless_stage_root_serials_[kGraphicsRootStage] = 1;
      }
    }
    current_bindless_stage_root_entries_[kGraphicsRootStage] = entries;
    current_bindless_stage_root_entries_valid_[kGraphicsRootStage] = true;
    current_bindless_stage_root_valid_[kGraphicsRootStage] = true;
    current_bindless_stage_root_frame_open_rebuild_pending_
        [kGraphicsRootStage] = false;
  } else {
    ++backend_telemetry_.bindless_root_reuse_hits[kGraphicsRootStage];
  }

  for (size_t stage = 0; stage < kStageCount; ++stage) {
    for (size_t cbv = 0; cbv < kCbvSlotCount; ++cbv) {
      const UniformBufferInfo::Cbv& uniform_cbv = uniforms.cbvs[stage][cbv];
      const bool cbv_active =
          uniforms.active_cbv_masks[stage] & (uint32_t(1) << cbv);
      current_bindless_cbv_gpu_addresses_[stage][cbv] =
          cbv_active ? (uniform_cbv.gpu_address ? uniform_cbv.gpu_address
                                                : null_buffer_->gpuAddress())
                     : null_buffer_->gpuAddress();
      current_bindless_cbv_buffers_[stage][cbv] =
          cbv_active ? uniform_cbv.buffer : nullptr;
      current_bindless_cbv_offsets_[stage][cbv] =
          cbv_active ? uniform_cbv.offset : 0;
    }
    current_bindless_active_cbv_masks_[stage] =
        uniforms.active_cbv_masks[stage];
  }
  current_bindless_stage_root_valid_[kStagePixel] = false;
  current_bindless_stage_root_frame_open_rebuild_pending_[kStagePixel] = false;
  current_bindless_stage_root_serials_[kStagePixel] = 0;
  current_bindless_shared_memory_is_uav_ = shared_memory_is_uav;

  PublishBindlessFixedResourceSet(shared_memory_usage);
  PublishBindlessRootResourceSet(uniforms);
  ApplyRenderEncoderResourceSets();

  const bool use_mesh_path =
      use_geometry_emulation || use_tessellation_emulation;
  const bool root_argument_path_needs_update =
      render_encoder_bindless_table_bind_mesh_path_ != use_mesh_path ||
      render_encoder_bindless_table_bind_tessellation_ !=
          use_tessellation_emulation;
  {
    constexpr size_t kGraphicsRootStage = kStageVertex;
    const StageRootArgumentAllocation& graphics_root_arguments =
        current_bindless_stage_root_arguments_[kGraphicsRootStage];
    const uint64_t graphics_root_serial =
        current_bindless_stage_root_serials_[kGraphicsRootStage];
    assert_true(graphics_root_arguments.valid);
    assert_true(graphics_root_arguments.upload_frame == frame_current_);
    const bool vertex_root_argument_binding_needs_update =
        render_encoder_bindless_stage_root_bind_serials_[kStageVertex] !=
            graphics_root_serial ||
        root_argument_path_needs_update;
    const bool pixel_root_argument_binding_needs_update =
        render_encoder_bindless_stage_root_bind_serials_[kStagePixel] !=
        graphics_root_serial;
    const bool root_argument_bindings_need_update =
        vertex_root_argument_binding_needs_update ||
        pixel_root_argument_binding_needs_update;
    if (use_mesh_path) {
      if (vertex_root_argument_binding_needs_update) {
        SetRenderEncoderObjectBuffer(graphics_root_arguments.buffer,
                                     graphics_root_arguments.offset,
                                     kIRArgumentBufferBindPoint);
        SetRenderEncoderMeshBuffer(graphics_root_arguments.buffer,
                                   graphics_root_arguments.offset,
                                   kIRArgumentBufferBindPoint);

        if (use_tessellation_emulation) {
          SetRenderEncoderObjectBuffer(graphics_root_arguments.buffer,
                                       graphics_root_arguments.offset,
                                       kIRArgumentBufferHullDomainBindPoint);
          SetRenderEncoderMeshBuffer(graphics_root_arguments.buffer,
                                     graphics_root_arguments.offset,
                                     kIRArgumentBufferHullDomainBindPoint);
        }
      }
      if (pixel_root_argument_binding_needs_update) {
        SetRenderEncoderFragmentBuffer(graphics_root_arguments.buffer,
                                       graphics_root_arguments.offset,
                                       kIRArgumentBufferBindPoint);
      }

      if (!heap_binds_set_on_encoder_) {
        SetRenderEncoderObjectBuffer(view_bindless_heap_, 0,
                                     kIRDescriptorHeapBindPoint);
        SetRenderEncoderMeshBuffer(view_bindless_heap_, 0,
                                   kIRDescriptorHeapBindPoint);
        SetRenderEncoderFragmentBuffer(view_bindless_heap_, 0,
                                       kIRDescriptorHeapBindPoint);
        SetRenderEncoderObjectBuffer(sampler_bindless_heap_, 0,
                                     kIRSamplerHeapBindPoint);
        SetRenderEncoderMeshBuffer(sampler_bindless_heap_, 0,
                                   kIRSamplerHeapBindPoint);
        SetRenderEncoderFragmentBuffer(sampler_bindless_heap_, 0,
                                       kIRSamplerHeapBindPoint);
        heap_binds_set_on_encoder_ = true;
      }
    } else {
      if (vertex_root_argument_binding_needs_update) {
        SetRenderEncoderVertexBuffer(graphics_root_arguments.buffer,
                                     graphics_root_arguments.offset,
                                     kIRArgumentBufferBindPoint);
      }
      if (pixel_root_argument_binding_needs_update) {
        SetRenderEncoderFragmentBuffer(graphics_root_arguments.buffer,
                                       graphics_root_arguments.offset,
                                       kIRArgumentBufferBindPoint);
      }

      if (!heap_binds_set_on_encoder_) {
        SetRenderEncoderVertexBuffer(view_bindless_heap_, 0,
                                     kIRDescriptorHeapBindPoint);
        SetRenderEncoderFragmentBuffer(view_bindless_heap_, 0,
                                       kIRDescriptorHeapBindPoint);
        SetRenderEncoderVertexBuffer(sampler_bindless_heap_, 0,
                                     kIRSamplerHeapBindPoint);
        SetRenderEncoderFragmentBuffer(sampler_bindless_heap_, 0,
                                       kIRSamplerHeapBindPoint);
        heap_binds_set_on_encoder_ = true;
      }
    }
    if (vertex_root_argument_binding_needs_update) {
      render_encoder_bindless_stage_root_bind_serials_[kStageVertex] =
          graphics_root_serial;
    }
    if (pixel_root_argument_binding_needs_update) {
      render_encoder_bindless_stage_root_bind_serials_[kStagePixel] =
          graphics_root_serial;
    }
    if (root_argument_bindings_need_update) {
      render_encoder_bindless_table_bind_mesh_path_ = use_mesh_path;
      render_encoder_bindless_table_bind_tessellation_ =
          use_tessellation_emulation;
    }
  }

  return true;
}

bool MetalCommandProcessor::PrepareGuestDMAIndexBufferForMemexport(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    PreparedIndexBuffer& prepared_index_buffer_out) {
  prepared_index_buffer_out = {};
  if (primitive_processing_result.index_buffer_type !=
      PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    return true;
  }
  if (!shared_memory_) {
    REXLOG_ERROR("IssueDraw: shared memory is unavailable for guest DMA index copy");
    return false;
  }

  MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
  if (!shared_mem_buffer) {
    REXLOG_ERROR("IssueDraw: shared memory buffer is unavailable for index copy");
    return false;
  }

  uint32_t index_stride = primitive_processing_result.host_index_format ==
                                  xenos::IndexFormat::kInt16
                              ? sizeof(uint16_t)
                              : sizeof(uint32_t);
  uint64_t index_bytes =
      uint64_t(primitive_processing_result.host_draw_vertex_count) *
      index_stride;
  uint64_t guest_index_base = primitive_processing_result.guest_index_base;
  if (guest_index_base > SharedMemory::kBufferSize ||
      SharedMemory::kBufferSize - guest_index_base < index_bytes) {
    REXLOG_WARN("Index buffer range out of bounds (base=0x{:08X} size={} count={})",
           static_cast<uint32_t>(guest_index_base), index_bytes,
           primitive_processing_result.host_draw_vertex_count);
    return false;
  }

  // Metal's buffer-to-buffer blit requires 4-byte offsets and sizes on macOS.
  uint64_t source_copy_offset = guest_index_base & ~uint64_t(3);
  uint64_t source_delta = guest_index_base - source_copy_offset;
  uint64_t copy_size = rex::align(source_delta + index_bytes, uint64_t(4));
  if (source_copy_offset > SharedMemory::kBufferSize ||
      SharedMemory::kBufferSize - source_copy_offset < copy_size) {
    REXLOG_WARN(
        "Aligned index buffer copy range out of bounds "
        "(base=0x{:08X} size={})",
        static_cast<uint32_t>(source_copy_offset), copy_size);
    return false;
  }
  if (!RequestSharedMemoryRange(SharedMemoryRequestReason::kIndexCopySource,
                                static_cast<uint32_t>(source_copy_offset),
                                static_cast<uint32_t>(copy_size))) {
    REXLOG_ERROR(
        "IssueDraw: failed to request guest DMA index copy range at 0x{:08X} "
        "(size {})",
        static_cast<uint32_t>(source_copy_offset), copy_size);
    return false;
  }

  MTL::CommandBuffer* command_buffer =
      RequestTransferCommandBuffer(TransferRequestSource::kGuestIndexCopy);
  if (!command_buffer) {
    REXLOG_ERROR("IssueDraw: failed to get command buffer for index copy");
    return false;
  }
  shared_memory_->MarkGpuAccess(static_cast<uint32_t>(source_copy_offset),
                                static_cast<uint32_t>(copy_size),
                                GetCurrentSubmission());

  MTL::Buffer* scratch_buffer = nullptr;
  size_t scratch_offset = 0;
  uint64_t scratch_gpu_address = 0;
  MTL::Buffer* direct_scratch_buffer = nullptr;
  if (constant_buffer_pool_ &&
      copy_size <= ui::GraphicsUploadBufferPool::kDefaultPageSize) {
    (void)constant_buffer_pool_->Request(
        frame_current_, static_cast<size_t>(copy_size), 4, &scratch_buffer,
        scratch_offset, scratch_gpu_address);
  }
  if (!scratch_buffer) {
    direct_scratch_buffer = device_->newBuffer(
        static_cast<NS::UInteger>(copy_size), MTL::ResourceStorageModePrivate);
    if (!direct_scratch_buffer) {
      REXLOG_ERROR(
          "IssueDraw: failed to allocate scratch index buffer for guest DMA "
          "memexport draw");
      return false;
    }
    scratch_buffer = direct_scratch_buffer;
    scratch_offset = 0;
  }

  MTL::BlitCommandEncoder* blit_encoder = pass_trace::Blit(command_buffer, "memexport_dma_index");
  if (!blit_encoder) {
    if (direct_scratch_buffer) {
      direct_scratch_buffer->release();
    }
    REXLOG_ERROR("IssueDraw: failed to create blit encoder for index copy");
    return false;
  }
  if (EncoderDebugLabelsEnabled()) {
    blit_encoder->setLabel(
        NS::String::string("XeniaGuestDMAIndexCopy", NS::UTF8StringEncoding));
  }
  if (!EncodeSharedMemoryBlitReadDependency(
          blit_encoder, static_cast<uint32_t>(source_copy_offset),
          static_cast<uint32_t>(copy_size))) {
    blit_encoder->endEncoding();
    if (direct_scratch_buffer) {
      direct_scratch_buffer->release();
    }
    return false;
  }
  blit_encoder->copyFromBuffer(
      shared_mem_buffer, static_cast<NS::UInteger>(source_copy_offset),
      scratch_buffer, static_cast<NS::UInteger>(scratch_offset),
      static_cast<NS::UInteger>(copy_size));
  blit_encoder->endEncoding();

  if (direct_scratch_buffer) {
    retired_memexport_index_buffers_.push_back(
        {direct_scratch_buffer, GetCurrentSubmission()});
  }
  prepared_index_buffer_out.buffer = scratch_buffer;
  prepared_index_buffer_out.offset = scratch_offset + source_delta;
  return true;
}

MetalCommandProcessor::PreparedDrawRenderTargetKey
MetalCommandProcessor::BuildPreparedDrawRenderTargetKey(
    const RegisterFile& regs, bool is_rasterization_done,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) const {
  PreparedDrawRenderTargetKey key;
  key.rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>().value;
  key.rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>().value;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    key.rb_color_info[i] =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i])
            .value;
  }
  key.normalized_depth_control = normalized_depth_control.value;
  key.normalized_color_mask = normalized_color_mask;
  key.is_rasterization_done = is_rasterization_done;
  return key;
}

bool MetalCommandProcessor::PreparedDrawQueueHasActiveZPD() const {
  // [occlusion-real] draws inside an open query must be encoded before its END.
  return zpd_active_report_ != 0;
}

void MetalCommandProcessor::RecordPreparedDrawQueueReject(
    PreparedDrawQueueRejectReason reject_reason) {
  const size_t reject_index = static_cast<size_t>(reject_reason);
  if (reject_index <
      backend_telemetry_.prepared_draw_queue_reject_reasons.size()) {
    ++backend_telemetry_.prepared_draw_queue_reject_reasons[reject_index];
  }
}

bool MetalCommandProcessor::CanQueuePreparedDraw(
    const PreparedDraw& draw,
    PreparedDrawQueueRejectReason& reject_reason) const {
  reject_reason = PreparedDrawQueueRejectReason::kNone;
  // A/B only (default off): FABLE2_NO_DRAW_QUEUE=1 materializes every draw
  // immediately instead of deferring guest-memory uploads to a queue flush.
  static const bool no_draw_queue = [] {
    const char* v = std::getenv("FABLE2_NO_DRAW_QUEUE");
    return v && v[0] == '1';
  }();
  if (no_draw_queue) {
    reject_reason = PreparedDrawQueueRejectReason::kQueueBudget;
    return false;
  }
  if (draw.memexport_used) {
    reject_reason = PreparedDrawQueueRejectReason::kMemexport;
    return false;
  }
  if (draw.has_pending_draw_pass_transfers) {
    reject_reason = PreparedDrawQueueRejectReason::kPendingDrawPassTransfers;
    return false;
  }
  if (PreparedDrawQueueHasActiveZPD()) {
    reject_reason = PreparedDrawQueueRejectReason::kZPDActive;
    return false;
  }
  if (draw.materialization_ranges.empty() && !draw.texture_upload_needed) {
    reject_reason = PreparedDrawQueueRejectReason::kNoSharedMemoryRanges;
    return false;
  }
  if (!draw.has_invalid_shared_memory && prepared_draw_queue_.empty() &&
      current_render_encoder_ && !draw.texture_upload_needed) {
    reject_reason = PreparedDrawQueueRejectReason::kResidentWithoutActiveQueue;
    return false;
  }
  if (prepared_draw_queue_render_target_key_valid_ &&
      draw.render_target_key != prepared_draw_queue_render_target_key_) {
    reject_reason = PreparedDrawQueueRejectReason::kRenderTargetKeyMismatch;
    return false;
  }

  size_t range_count = draw.materialization_ranges.size();
  uint64_t byte_count = 0;
  for (const SharedMemory::Range& range : draw.materialization_ranges) {
    byte_count += range.length;
  }
  if (DrawPrepLiteEnabled()) {
    // [draw-prep-lite] running totals of the queued draws (kept at push and
    // queue take; queued records are not modified).
    range_count += prepared_draw_queue_range_total_;
    byte_count += prepared_draw_queue_byte_total_;
  } else {
    for (const PreparedDrawPtr& queued_draw : prepared_draw_queue_) {
      range_count += queued_draw->materialization_ranges.size();
      for (const SharedMemory::Range& range :
           queued_draw->materialization_ranges) {
        byte_count += range.length;
      }
    }
  }
  if (prepared_draw_queue_.size() + 1 > kPreparedDrawQueueMaxDraws ||
      range_count > kPreparedDrawQueueMaxRanges ||
      byte_count > kPreparedDrawQueueMaxBytes) {
    reject_reason = PreparedDrawQueueRejectReason::kQueueBudget;
    return false;
  }
  return true;
}

bool MetalCommandProcessor::EncodePreparedDraw(const PreparedDraw& draw) {
  // [tile-resolve extent] the rows this draw can write (its scissor, already
  // clamped to the attachments). A pass opened with a shorter render area
  // ends here when the draw needs more rows or transfers are to be drawn,
  // and the next pass opens tall enough (docs/tile-resolve-20261013.md).
  uint32_t need_x0 = 0, need_y0 = 0, need_x1 = 65536, need_y1 = 65536;
  if (draw.prepare_uniforms) {
    const MTL::ScissorRect& sc = draw.dynamic_state.scissor;
    need_x0 = uint32_t(sc.x);
    need_y0 = uint32_t(sc.y);
    need_x1 = uint32_t(sc.x + sc.width);
    need_y1 = uint32_t(sc.y + sc.height);
  }
  if (current_render_encoder_ && pass_extent_h_ &&
      (need_y1 > pass_extent_h_ ||
       (render_target_cache_ &&
        render_target_cache_->HasPendingDrawPassTransfers()))) {
    ++UploadTrace().tres_grow;
    render_end_detail_ = "extent-grow";
    EndRenderEncoder(
        RenderEncoderEndReason::kBeginRenderEncoderDescriptorChanged);
  }
  if (TileResolveKeptOpen() && TileResolveKeptHazard(draw)) {
    ++UploadTrace().tres_kept_break;
    render_end_detail_ = "tile-kept-hazard";
    EndRenderEncoder(
        RenderEncoderEndReason::kBeginRenderEncoderDescriptorChanged);
  }
  pass_open_need_bottom_ = need_y1;
  const bool draw_pass_transfers =
      render_target_cache_ && render_target_cache_->HasPendingDrawPassTransfers();
  if (!BeginRenderEncoderForDraw(draw.fallback_depth_attachment_required)) {
    static bool no_command_buffer_logged = false;
    if (!no_command_buffer_logged) {
      no_command_buffer_logged = true;
      REXLOG_ERROR(
          "IssueDraw: failed to begin Metal command buffer/render encoder; "
          "skipping draws until uniforms buffer allocation recovers");
    }
    return false;
  }
  if (render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers()) {
    if (render_target_cache_->HasPendingDrawPassTransferQuads()) {
      NoteDrawPassTransferInheritedRasterizerState();
    }
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask
        transfer_mutations =
            MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
    if (!render_target_cache_->EncodePendingDrawPassTransfers(
            current_render_encoder_, current_render_pass_descriptor_,
            &transfer_mutations)) {
      if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
        return false;
      }
      if (!BeginRenderEncoderForDraw(draw.fallback_depth_attachment_required)) {
        return false;
      }
      transfer_mutations =
          MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
    }
    InvalidateRenderEncoderStateAfterDrawPassTransfers(transfer_mutations);
  }
  if (draw_pass_transfers) {
    PassExtentNoteDrawn(0, 0, 65536, 65536);
  }
  PassExtentNoteDrawn(need_x0, need_y0, need_x1, need_y1);
  pass_draws_y1_ = std::max(pass_draws_y1_, need_y1);
  if (tres_kept_gen_ == render_encoder_gen_ &&
      tres_kept_counted_gen_ != render_encoder_gen_) {
    // A draw continued the pass after a tile resolve (KEEP_OPEN).
    tres_kept_counted_gen_ = render_encoder_gen_;
    ++UploadTrace().tres_kept;
  }

  if (draw.shared_memory_hazard_range_count &&
      PendingSharedMemoryWritesOverlapRanges(
          draw.shared_memory_hazard_ranges.data(),
          draw.shared_memory_hazard_range_count)) {
    if (!EncodeSharedMemoryRenderReadDependencies(
            draw.shared_memory_hazard_ranges.data(),
            draw.shared_memory_hazard_range_count,
            draw.shared_memory_consumer_stages)) {
      return false;
    }
    if (!current_render_encoder_) {
      return false;
    }
  }
  if (shared_memory_ && draw.shared_memory_hazard_range_count) {
    uint64_t submission = GetCurrentSubmission();
    for (uint32_t i = 0; i < draw.shared_memory_hazard_range_count; ++i) {
      const SharedMemoryRange& range = draw.shared_memory_hazard_ranges[i];
      shared_memory_->MarkGpuAccess(range.start, range.length, submission);
    }
  }

  if (draw.trace_ps) {
    pass_trace::NoteShader(current_render_pass_trace_, draw.trace_ps);
  }
  if (current_render_pipeline_state_ != draw.pipeline) {
    current_render_encoder_->setRenderPipelineState(draw.pipeline);
    current_render_pipeline_state_ = draw.pipeline;
    ++backend_telemetry_.pipeline_sets;
  } else {
    ++backend_telemetry_.pipeline_set_skips;
  }
  if (draw.use_tessellation_emulation) {
    if (!tessellator_tables_buffer_) {
      REXLOG_ERROR("Tessellation emulation requires tessellator tables buffer");
      return false;
    }
    SetRenderEncoderObjectBuffer(tessellator_tables_buffer_, 0,
                                 kIRRuntimeTessellatorTablesBindPoint);
    SetRenderEncoderMeshBuffer(tessellator_tables_buffer_, 0,
                               kIRRuntimeTessellatorTablesBindPoint);
    UseRenderEncoderResource(tessellator_tables_buffer_,
                             MTL::ResourceUsageRead);
  }

  if (draw.prepare_uniforms) {
    ApplyDrawDynamicState(draw.dynamic_state);

    RestoreBindlessTextureResourceSet(draw.texture_resource_set);
    if (MetalRenderTargetCache::TileResolveEnabled()) {
      TileResolveNotePassSampled(draw.texture_resource_set);
    }

    if (!PopulateBindlessTables(
            draw.shared_memory_is_uav, draw.shared_memory_usage,
            draw.use_geometry_emulation, draw.use_tessellation_emulation,
            draw.uniforms)) {
      return false;
    }
  }


  if (!draw.capture_label.empty()) {
    current_render_encoder_->insertDebugSignpost(NS::String::string(
        draw.capture_label.c_str(), NS::UTF8StringEncoding));
  }
  dispatch_native_tess_ = draw.native_tess ? &draw : nullptr;
  dispatch_native_points_ = draw.native_points;
  dispatch_native_geometry_vertices_ = draw.native_geometry_vertices;
  dispatch_native_points_mesh_ = draw.native_points_mesh;
  ZpdBeginDrawSpan();
  const bool dispatched = DispatchDraw(
      draw.primitive_processing_result, draw.use_tessellation_emulation,
      draw.tessellation_pipeline_state, draw.use_geometry_emulation,
      draw.geometry_pipeline_state, draw.shared_memory_is_uav,
      draw.shared_memory_usage, draw.memexport_used,
      draw.memexport_write_stages, draw.uses_vertex_fetch,
      draw.prepare_uniforms,
      draw.prepared_guest_dma_index_buffer.buffer
          ? &draw.prepared_guest_dma_index_buffer
          : nullptr,
      draw.has_vertex_bindings, draw.vertex_ranges.data(),
      draw.vertex_range_count,
      draw.has_index_buffer_info ? &draw.index_buffer_info : nullptr,
      draw.memexport_ranges);
  ZpdEndDrawSpan(dispatched);
  return dispatched;
}

bool MetalCommandProcessor::TexturePrecommitEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_TEXTURE_PRECOMMIT");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[texture-precommit] {} (FABLE2_TEXTURE_PRECOMMIT={})",
        on ? "on: texture loads that no draw of the open command buffer has "
             "sampled and whose source is not GPU-written in it go to the "
             "pre-commit command buffer instead of ending the render pass"
           : "off: every texture load is encoded in the current command buffer",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalCommandProcessor::SharedMemoryRangeHasPendingGpuWrite(
    uint32_t start, uint32_t length) const {
  // Only writes of the open command buffer: those of committed ones finish
  // before the pre-commit command buffer starts (its main_cb_event_ wait).
  SharedMemoryRange range;
  range.start = start;
  range.length = length;
  const uint64_t committed = GetLastCommittedSubmission();
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (pending.submission_id > committed &&
        PendingSharedMemoryWriteOverlapsRanges(pending, &range, 1)) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::TexturePrecommitAllowed() const {
  return TexturePrecommitEnabled() && texture_cache_ && main_cb_event_ &&
         precommit_event_ && precommit_fence_ && current_command_buffer_ &&
         current_render_encoder_ && !shared_memory_upload_blit_encoder_ &&
         !texture_precommit_active_ &&
         texture_cache_->CanUploadToPrecommit();
}

bool MetalCommandProcessor::BeginTexturePrecommit() {
  if (!TexturePrecommitAllowed() || !EnsurePrecommitCommandBuffer()) {
    return false;
  }
  // The texture cache opens its own encoders in the pre-commit command
  // buffer; staged copies opened later in it get a new blit encoder.
  EndPrecommitBlit();
  texture_precommit_active_ = true;
  render_end_detail_ = nullptr;  // the pass is not ended by this flush
  texture_cache_->SetPrecommitTarget(precommit_cb_);
  ++UploadTrace().tex_pre_flush;
  return true;
}

void MetalCommandProcessor::EndTexturePrecommit() {
  if (!texture_precommit_active_) {
    return;
  }
  texture_precommit_active_ = false;
  texture_cache_->SetPrecommitTarget(nullptr);
  if (texture_cache_->HasPendingPrecommitVerify()) {
    // FABLE2_TEXTURE_PRECOMMIT_VERIFY (trace-only): copy the same source
    // ranges here, at the position the in-command-buffer load would have
    // used; this ends the render pass (verify runs are not perf runs).
    if (current_render_encoder_) {
      render_end_detail_ = "texture:verify";
      EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
    }
    texture_cache_->EncodePrecommitVerifyReferences(current_command_buffer_);
  }
  // RAW: the open render pass (if the flush kept it open) waits for the
  // pre-commit encoders; later encoders wait on the pre-commit event
  // (EncodePrecommitWait at EndRenderEncoder / EncodeMainCommandBufferEnd).
  WaitPrecommitInRenderEncoder();
}

void MetalCommandProcessor::AbortTexturePrecommit(const char* why) {
  if (!texture_precommit_active_) {
    return;
  }
  // Loads already encoded stay in the pre-commit command buffer (they were
  // eligible); the rest of the flush runs in the current command buffer,
  // after the render pass ends as it did before this route existed.
  texture_precommit_active_ = false;
  texture_cache_->SetPrecommitTarget(nullptr);
  ++UploadTrace().tex_pre_fallback;
  WaitPrecommitInRenderEncoder();
  if (current_render_encoder_) {
    ++UploadTrace().tex_brk;
    render_end_detail_ = why;
    EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
  }
  if (texture_cache_->HasPendingPrecommitVerify()) {
    // The loads encoded so far would have run here; the rest of the flush
    // (e.g. the upload that caused this fallback) comes after.
    texture_cache_->EncodePrecommitVerifyReferences(current_command_buffer_);
  }
}

bool MetalCommandProcessor::ClassifyTextureMaterialization(
    const MetalTextureCache::TextureMaterializationPlan* const* plans,
    size_t count) {
  if (!texture_cache_ || !plans || !count) {
    return false;
  }
  const bool render_pass_open = current_render_encoder_ != nullptr;
  const bool trace = gpu_time_trace::Enabled();
  if (!trace && (!render_pass_open || !TexturePrecommitEnabled())) {
    return false;  // nothing to decide and nothing to count
  }
  MetalTextureCache::TextureFlushClass c;
  std::vector<const void*> seen;
  for (size_t i = 0; i < count; ++i) {
    texture_cache_->ClassifyMaterializationPlan(*plans[i], render_pass_open,
                                                trace, seen, c);
  }
  // [tex-rename] a flush whose planned loads are all no-ops takes the
  // pre-commit route too (nothing is written; the guard covers a change).
  const bool all_eligible =
      (c.loads || c.noop) && c.eligible == c.loads;
  if (render_pass_open && !all_eligible && c.renamed) {
    UploadTrace().trn_wasted += c.renamed;
  }
  if (trace && render_pass_open) {
    // Pass-trace end detail (f2diag gpu groups texture:* as texture_upload).
    UploadTraceCounters& u = UploadTrace();
    if (all_eligible) {
      ++u.tex_brk_elig;
      render_end_detail_ = "texture:eligible";
    } else if (c.sampled_current) {
      ++u.tex_brk_blk_samp;
      if (c.eligible + c.sampled_only == c.loads) ++u.tex_brk_samp_only;
      render_end_detail_ = "texture:sampled_current";
    } else if (c.resident_touched) {
      ++u.tex_brk_blk_res;
      render_end_detail_ = "texture:resident_current";
    } else {
      ++u.tex_brk_blk_other;
      render_end_detail_ = "texture:other";
    }
  }
  return all_eligible;
}

bool MetalCommandProcessor::FlushPreparedDrawQueue(
    PreparedDrawFlushReason reason) {
  if (prepared_draw_queue_.empty()) {
    return true;
  }
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index <
      backend_telemetry_.prepared_draw_queue_flush_reasons.size()) {
    ++backend_telemetry_.prepared_draw_queue_flush_reasons[reason_index];
  }
  ++backend_telemetry_.prepared_draw_queue_flushes;
  if (prepared_draw_queue_.size() == 1) {
    ++backend_telemetry_.prepared_draw_queue_single_draw_flushes;
  }

  std::vector<PreparedDrawPtr> draws = std::move(prepared_draw_queue_);
  prepared_draw_queue_.clear();
  prepared_draw_queue_range_total_ = 0;
  prepared_draw_queue_byte_total_ = 0;
  // [prepared-draw-pool] give the queue reserved storage back and recycle the flushed
  // entries (and this vector's storage) on every exit path.
  struct PreparedDrawRecycler {
    MetalCommandProcessor* self;
    std::vector<PreparedDrawPtr>* draws;
    ~PreparedDrawRecycler() {
      if (self) self->RecyclePreparedDraws(*draws);
    }
  } prepared_draw_recycler{PreparedDrawPoolEnabled() ? this : nullptr, &draws};
  if (PreparedDrawPoolEnabled() && !prepared_draw_queue_storage_spares_.empty() &&
      prepared_draw_queue_.capacity() == 0) {
    prepared_draw_queue_ = std::move(prepared_draw_queue_storage_spares_.back());
    prepared_draw_queue_storage_spares_.pop_back();
  }
  prepared_draw_queue_render_target_key_valid_ = false;
  prepared_draw_queue_render_target_key_ = {};

  std::vector<SharedMemory::Range> ranges;
  size_t range_count = 0;
  uint64_t byte_count = 0;
  bool has_invalid_shared_memory = false;
  bool has_texture_materialization = false;
  for (const PreparedDrawPtr& draw_ptr : draws) {
    const PreparedDraw& draw = *draw_ptr;
    range_count += draw.materialization_ranges.size();
    has_invalid_shared_memory =
        has_invalid_shared_memory || draw.has_invalid_shared_memory;
    has_texture_materialization =
        has_texture_materialization ||
        draw.texture_materialization_plan.NeedsTextureUpload();
    for (const SharedMemory::Range& range : draw.materialization_ranges) {
      byte_count += range.length;
    }
  }
  ranges.reserve(range_count);
  for (const PreparedDrawPtr& draw : draws) {
    ranges.insert(ranges.end(), draw->materialization_ranges.begin(),
                  draw->materialization_ranges.end());
  }
  backend_telemetry_.prepared_draw_queue_draws_flushed += draws.size();
  backend_telemetry_.prepared_draw_queue_ranges_flushed += ranges.size();
  backend_telemetry_.prepared_draw_queue_bytes_flushed += byte_count;
  if (has_invalid_shared_memory) {
    ++backend_telemetry_.prepared_draw_queue_invalid_flushes;
  }

  const bool previous_flushing = flushing_prepared_draw_queue_;
  flushing_prepared_draw_queue_ = true;

  if (has_invalid_shared_memory && shared_memory_ && !ranges.empty()) {
    PrepareSharedMemoryUploadBeforeDrawPass(
        ranges.data(), static_cast<uint32_t>(ranges.size()));
    if (!RequestSharedMemoryRanges(
            SharedMemoryRequestReason::kDrawMaterialization, ranges.data(),
            static_cast<uint32_t>(ranges.size()))) {
      REXLOG_ERROR("Failed to request {} prepared-draw shared-memory ranges",
             ranges.size());
      flushing_prepared_draw_queue_ = previous_flushing;
      return false;
    }
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kMaterializationDrain);
  }

  if (has_texture_materialization && texture_cache_) {
    bool texture_precommit = false;
    {
      std::vector<const MetalTextureCache::TextureMaterializationPlan*> plans;
      plans.reserve(draws.size());
      for (const PreparedDrawPtr& draw : draws) {
        if (draw->texture_materialization_plan.NeedsTextureUpload()) {
          plans.push_back(&draw->texture_materialization_plan);
        }
      }
      texture_precommit =
          ClassifyTextureMaterialization(plans.data(), plans.size()) &&
          BeginTexturePrecommit();
    }
    const bool texture_break = current_render_encoder_ != nullptr;
    const uint64_t loads_before = texture_cache_->loaded_texture_data_count();
    if (current_render_encoder_ && !texture_precommit) {
      ++UploadTrace().tex_brk;
      EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
    }
    if (!EnsureCommandBuffer()) {
      EndTexturePrecommit();
      flushing_prepared_draw_queue_ = previous_flushing;
      return false;
    }
    texture_cache_->BeginTextureUploadBatch();
    bool texture_materialization_succeeded = true;
    for (PreparedDrawPtr& draw_ptr : draws) {
      PreparedDraw& draw = *draw_ptr;
      if (!draw.texture_materialization_plan.NeedsTextureUpload()) {
        continue;
      }
      ++backend_telemetry_.prepared_draw_queue_texture_plans_flushed;
      backend_telemetry_.prepared_draw_queue_texture_loads_planned +=
          draw.texture_materialization_plan.planned_load_count;
      if (!texture_cache_->ExecuteTextureMaterialization(
              draw.texture_materialization_plan)) {
        texture_materialization_succeeded = false;
        break;
      }
      backend_telemetry_.prepared_draw_queue_texture_loads_executed +=
          draw.texture_materialization_plan.executed_load_count;
    }
    if (!texture_cache_->EndTextureUploadBatch()) {
      texture_materialization_succeeded = false;
    }
    if (texture_precommit) {
      if (texture_precommit_active_) {
        UploadTrace().tex_pre_ld += uint32_t(
            texture_cache_->loaded_texture_data_count() - loads_before);
      }
      EndTexturePrecommit();
    }
    if (texture_break) {
      UploadTrace().tex_ld_exec += uint32_t(
          texture_cache_->loaded_texture_data_count() - loads_before);
    }
    if (MetalTextureCache::TexRenameEnabled() &&
        texture_cache_->TexRenameHasOrphans()) {
      std::vector<const MetalTextureCache::TextureMaterializationPlan*> plans;
      for (const PreparedDrawPtr& draw : draws) {
        plans.push_back(&draw->texture_materialization_plan);
      }
      TexRenameAfterLoads(plans.data(), plans.size());
    }
    if (!texture_materialization_succeeded) {
      flushing_prepared_draw_queue_ = previous_flushing;
      return false;
    }
  }

  for (const PreparedDrawPtr& draw : draws) {
    if (!EncodePreparedDraw(*draw)) {
      flushing_prepared_draw_queue_ = previous_flushing;
      return false;
    }
  }

  flushing_prepared_draw_queue_ = previous_flushing;
  return true;
}

bool MetalCommandProcessor::PreparedDrawPoolEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PREPARED_DRAW_POOL");
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[prepared-draw-pool] {} (FABLE2_PREPARED_DRAW_POOL)", e ? "on" : "off");
    return e;
  }();
  return on;
}

void MetalCommandProcessor::ResetPreparedDrawKeepCapacity(PreparedDraw& draw) {
  // Default every field (new fields included), then hand the emptied vectors back.
  std::string capture_label = std::move(draw.capture_label);
  std::vector<Shader::VertexBinding> vertex_bindings = std::move(draw.vertex_bindings);
  std::vector<SharedMemory::Range> materialization_ranges =
      std::move(draw.materialization_ranges);
  std::vector<draw_util::MemExportRange> memexport_ranges = std::move(draw.memexport_ranges);
  std::vector<RenderResourceRef> texture_resources =
      std::move(draw.texture_resource_set.resources);
  MetalTextureCache::TextureMaterializationPlan plan =
      std::move(draw.texture_materialization_plan);
  draw = PreparedDraw{};
  capture_label.clear();
  // vertex_bindings keeps its elements: IssueDrawImpl always copy-assigns it before the
  // draw is submitted, and assigning over existing elements reuses their nested vectors.
  materialization_ranges.clear();
  memexport_ranges.clear();
  texture_resources.clear();
  plan.Reset();
  draw.capture_label = std::move(capture_label);
  draw.vertex_bindings = std::move(vertex_bindings);
  draw.materialization_ranges = std::move(materialization_ranges);
  draw.memexport_ranges = std::move(memexport_ranges);
  draw.texture_resource_set.resources = std::move(texture_resources);
  draw.texture_materialization_plan = std::move(plan);
}

bool FlatPtrSetLite2Enabled() {
  // Same switch as DrawPrepLite2Enabled (FABLE2_DRAW_PREP_LITE2), read once.
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_DRAW_PREP_LITE2");  // getenv-ok: read once
    return !(v && v[0] == '0');
  }();
  return on;
}

bool FlatPtrSetEnabled() {
  // Same switch as DrawPrepLiteEnabled (FABLE2_DRAW_PREP_LITE), read once.
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_DRAW_PREP_LITE");  // getenv-ok: read once
    return !(v && v[0] == '0');
  }();
  return on;
}

bool MetalCommandProcessor::DrawPrepLiteEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_DRAW_PREP_LITE");  // getenv-ok: read once
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[draw-prep-lite] {} (FABLE2_DRAW_PREP_LITE)", e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalCommandProcessor::DrawPrepLite2Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_DRAW_PREP_LITE2");  // getenv-ok: read once
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[draw-prep-lite2] {} (FABLE2_DRAW_PREP_LITE2)", e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalCommandProcessor::PreparedDrawSlimEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PREPARED_DRAW_SLIM");
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[prepared-draw-slim] {} (FABLE2_PREPARED_DRAW_SLIM)", e ? "on" : "off");
    return e;
  }();
  return on;
}

// Every field of PreparedDraw except the fixed arrays whose live length is a count reset here
// (shared_memory_hazard_ranges, vertex_ranges: every reader stops at the count) and the
// uniforms (PrepareDrawConstants assigns them whole; IssueDrawImpl resets them when it does
// not prepare uniforms). vertex_bindings keeps its elements (IssueDrawImpl always assigns it).
// A new PreparedDraw field must be reset here too.
void MetalCommandProcessor::ResetPreparedDrawSlim(PreparedDraw& draw) {
  draw.capture_label.clear();
  draw.trace_ps = 0;
  draw.pipeline = nullptr;
  draw.tessellation_pipeline_state = nullptr;
  draw.geometry_pipeline_state = nullptr;
  draw.primitive_processing_result = {};
  draw.dynamic_state = {};
  draw.prepared_guest_dma_index_buffer = {};
  draw.index_buffer_info = {};
  draw.has_index_buffer_info = false;
  draw.vertex_range_count = 0;
  draw.has_vertex_bindings = false;
  draw.materialization_ranges.clear();
  draw.has_invalid_shared_memory = false;
  draw.shared_memory_hazard_range_count = 0;
  draw.shared_memory_consumer_stages = MTL::RenderStages(0);
  draw.memexport_ranges.clear();
  draw.memexport_write_stages = MTL::RenderStages(0);
  draw.shared_memory_usage = MTL::ResourceUsageRead;
  draw.render_target_key = {};
  draw.texture_resource_set.resources.clear();
  draw.texture_resource_set.serial = 0;
  draw.texture_resource_set.source_serial = 0;
  draw.texture_materialization_plan.Reset();
  draw.use_tessellation_emulation = false;
  draw.use_geometry_emulation = false;
  draw.native_tess = false;
  draw.native_tess_cpu = false;
  draw.tess_runs.clear();
  draw.tess_merged_indices = nullptr;
  draw.tess_merged_offset = 0;
  draw.tess_merged_count = 0;
  draw.native_points = false;
  draw.native_geometry_vertices = 1;
  draw.native_points_mesh = false;
  draw.tess_factor_buffer = nullptr;
  draw.tess_factor_offset = 0;
  draw.tess_patch_count = 0;
  draw.shared_memory_is_uav = false;
  draw.memexport_used = false;
  draw.uses_vertex_fetch = false;
  draw.prepare_uniforms = false;
  draw.fallback_depth_attachment_required = false;
  draw.texture_upload_needed = false;
  draw.may_texture_request_load_data = false;
  draw.has_pending_draw_pass_transfers = false;
}

std::unique_ptr<MetalCommandProcessor::PreparedDraw>
MetalCommandProcessor::AcquirePreparedDraw() {
  if (!PreparedDrawPoolEnabled() || prepared_draw_free_.empty()) {
    return std::make_unique<PreparedDraw>();
  }
  std::unique_ptr<PreparedDraw> draw = std::move(prepared_draw_free_.back());
  prepared_draw_free_.pop_back();
  if (PreparedDrawSlimEnabled()) {
    ResetPreparedDrawSlim(*draw);
  } else {
    ResetPreparedDrawKeepCapacity(*draw);
  }
  return draw;
}

void MetalCommandProcessor::ReleasePreparedDraw(std::unique_ptr<PreparedDraw> draw) {
  // Queue budget plus a few records for draws issued while a flush encodes.
  constexpr size_t kMaxFreeDraws = kPreparedDrawQueueMaxDraws + 8;
  if (draw && PreparedDrawPoolEnabled() && prepared_draw_free_.size() < kMaxFreeDraws) {
    prepared_draw_free_.push_back(std::move(draw));
  }
}

void MetalCommandProcessor::RecyclePreparedDraws(
    std::vector<std::unique_ptr<PreparedDraw>>& draws) {
  constexpr size_t kMaxSpareQueueStorage = 2;
  for (std::unique_ptr<PreparedDraw>& draw : draws) {
    ReleasePreparedDraw(std::move(draw));
  }
  draws.clear();
  if (draws.capacity() && prepared_draw_queue_storage_spares_.size() < kMaxSpareQueueStorage) {
    prepared_draw_queue_storage_spares_.push_back(std::move(draws));
  }
}

MetalCommandProcessor::RenderResourceSet&
MetalCommandProcessor::AcquireResourceSetScratch(RenderResourceSet& fallback) {
  if (!PreparedDrawPoolEnabled()) {
    return fallback;
  }
  resource_set_scratch_.resources.clear();
  resource_set_scratch_.serial = 0;
  resource_set_scratch_.source_serial = 0;
  return resource_set_scratch_;
}

bool MetalCommandProcessor::SubmitPreparedDraw(
    std::unique_ptr<PreparedDraw> draw_ptr) {
  PreparedDrawQueueRejectReason reject_reason =
      PreparedDrawQueueRejectReason::kNone;
  if (CanQueuePreparedDraw(*draw_ptr, reject_reason)) {
    if (prepared_draw_queue_.empty()) {
      prepared_draw_queue_render_target_key_ = draw_ptr->render_target_key;
      prepared_draw_queue_render_target_key_valid_ = true;
    }
    prepared_draw_queue_range_total_ += draw_ptr->materialization_ranges.size();
    for (const SharedMemory::Range& range : draw_ptr->materialization_ranges) {
      prepared_draw_queue_byte_total_ += range.length;
    }
    prepared_draw_queue_.push_back(std::move(draw_ptr));
    ++backend_telemetry_.prepared_draw_queue_appends;
    if (prepared_draw_queue_.size() >= kPreparedDrawQueueMaxDraws) {
      return FlushPreparedDrawQueue(PreparedDrawFlushReason::kQueueBudget);
    }
    return true;
  }

  // Not queued: the record goes back to the pool on every exit path.
  struct PreparedDrawLease {
    MetalCommandProcessor* self;
    std::unique_ptr<PreparedDraw> ptr;
    ~PreparedDrawLease() { self->ReleasePreparedDraw(std::move(ptr)); }
  } lease{this, std::move(draw_ptr)};
  PreparedDraw& draw = *lease.ptr;
  RecordPreparedDrawQueueReject(reject_reason);
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kQueueReject)) {
    return false;
  }

  if (draw.has_invalid_shared_memory && shared_memory_ &&
      !draw.materialization_ranges.empty()) {
    PrepareSharedMemoryUploadBeforeDrawPass(
        draw.materialization_ranges.data(),
        static_cast<uint32_t>(draw.materialization_ranges.size()));
    if (!RequestSharedMemoryRanges(
            SharedMemoryRequestReason::kDrawMaterialization,
            draw.materialization_ranges.data(),
            static_cast<uint32_t>(draw.materialization_ranges.size()))) {
      REXLOG_ERROR("Failed to request {} current-draw shared-memory ranges",
             draw.materialization_ranges.size());
      return false;
    }
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kMaterializationDrain);
  }

  if (draw.texture_materialization_plan.NeedsTextureUpload() &&
      texture_cache_) {
    bool texture_precommit = false;
    {
      const MetalTextureCache::TextureMaterializationPlan* plan =
          &draw.texture_materialization_plan;
      texture_precommit =
          ClassifyTextureMaterialization(&plan, 1) && BeginTexturePrecommit();
    }
    const bool texture_break = current_render_encoder_ != nullptr;
    const uint64_t loads_before = texture_cache_->loaded_texture_data_count();
    if (current_render_encoder_ && !texture_precommit) {
      ++UploadTrace().tex_brk;
      EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
    }
    if (!EnsureCommandBuffer()) {
      EndTexturePrecommit();
      return false;
    }
    const bool texture_loaded = texture_cache_->ExecuteTextureMaterialization(
        draw.texture_materialization_plan);
    if (texture_precommit) {
      if (texture_precommit_active_) {
        UploadTrace().tex_pre_ld += uint32_t(
            texture_cache_->loaded_texture_data_count() - loads_before);
      }
      EndTexturePrecommit();
    }
    {
      const MetalTextureCache::TextureMaterializationPlan* plan =
          &draw.texture_materialization_plan;
      TexRenameAfterLoads(&plan, 1);
    }
    if (!texture_loaded) {
      return false;
    }
    if (texture_break) {
      UploadTrace().tex_ld_exec += uint32_t(
          texture_cache_->loaded_texture_data_count() - loads_before);
    }
  }

  return EncodePreparedDraw(draw);
}

const MetalCommandProcessor::TessPattern* MetalCommandProcessor::GetTessPattern(
    const float factors[6]) {
  uint32_t bits[6];
  std::memcpy(bits, factors, sizeof(bits));
  const uint64_t key = XXH3_64bits(bits, sizeof(bits));
  auto it = tess_patterns_.find(key);
  if (it != tess_patterns_.end()) {
    return std::memcmp(it->second.factor_bits, bits, sizeof(bits)) == 0
               ? &it->second
               : nullptr;
  }
  // Patterns are kept for the session (in-flight draws reference them); a
  // pathological number of factor combinations falls back to the other path.
  constexpr size_t kMaxPatterns = 16384;
  if (tess_patterns_.size() >= kMaxPatterns) {
    return nullptr;
  }
  std::vector<ref_tess::Vertex> vertices;
  std::vector<uint32_t> indices;
  ref_tess::TessellateQuadFractionalEven(factors, vertices, indices);
  if (vertices.empty() || indices.empty()) {
    return nullptr;
  }
  if (!tess_arena_) {
    tess_arena_ = device_->newBuffer(size_t(kTessArenaPoints) * sizeof(float) * 2,
                                     MTL::ResourceStorageModeShared);
    if (!tess_arena_) {
      return nullptr;
    }
    tess_arena_->setLabel(
        NS::String::string("NativeTessArena", NS::UTF8StringEncoding));
  }
  if (tess_arena_used_ + vertices.size() > kTessArenaPoints) {
    return nullptr;
  }
  TessPattern pattern;
  std::memcpy(pattern.factor_bits, bits, sizeof(bits));
  pattern.arena_base = tess_arena_used_;
  std::memcpy(static_cast<float*>(tess_arena_->contents()) +
                  size_t(tess_arena_used_) * 2,
              vertices.data(), vertices.size() * sizeof(float) * 2);
  tess_arena_used_ += uint32_t(vertices.size());
  pattern.indices = device_->newBuffer(indices.data(),
                                       indices.size() * sizeof(uint32_t),
                                       MTL::ResourceStorageModeShared);
  pattern.index_count = uint32_t(indices.size());
  pattern.local_indices = std::move(indices);
  if (!pattern.indices) {
    return nullptr;
  }
  return &tess_patterns_.emplace(key, pattern).first->second;
}

void MetalCommandProcessor::GetTessellationFactorRange(const RegisterFile& regs,
                                                       float& min_out,
                                                       float& max_out) {
  min_out = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  max_out = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  if (REXCVAR_GET(metal_fable_terrain_uniform_tessellation) &&
      !TerrainAdaptiveMode() && active_vertex_shader() &&
      active_vertex_shader()->ucode_data_hash() ==
          UINT64_C(0x5003700B7C9B1C16)) {
    // Work around terrain depth gaps seen with adaptive edge factors on Metal.
    // Clamp this terrain shader to the guest maximum so neighboring patches
    // use uniform subdivision; leave other tessellated geometry unchanged.
    min_out = max_out;
  }
}

bool MetalCommandProcessor::NativeTessTraceEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NATIVE_TESS_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}

void MetalCommandProcessor::NativeTessTrace(uint64_t vs, uint64_t ps,
                                            const char* path, uint32_t mode,
                                            bool blended, uint32_t patches,
                                            float factor_min, float factor_max,
                                            bool bad_factor, uint32_t zfunc,
                                            uint32_t z_write) {
  static bool device_logged = false;
  if (!device_logged) {
    device_logged = true;
    REXLOG_INFO(
        "[tess-trace] device {} apple7 {} apple8 {} apple9 {} apple10 {}; "
        "tessellator fractional-even, half factors, CCW output, max 64; "
        "FABLE2_NATIVE_TESS={} blended={} terrain-adaptive={} snap-uv={}",
        device_->name()->utf8String(),
        device_->supportsFamily(MTL::GPUFamilyApple7) ? 1 : 0,
        device_->supportsFamily(MTL::GPUFamilyApple8) ? 1 : 0,
        device_->supportsFamily(MTL::GPUFamilyApple9) ? 1 : 0,
        device_->supportsFamily(MTL::GPUFamily(1010)) ? 1 : 0,
        NativeTessMode(), NativeTessBlendedMode() ? 1 : 0,
        TerrainAdaptiveMode() ? 1 : 0, NativeTessSnapUvMode() ? 1 : 0);
  }
  static std::unordered_set<uint64_t> seen;
  static std::map<std::string, uint64_t> counts;
  static uint64_t window_start = frame_current_;
  const uint64_t key_data[] = {vs, ps, uint64_t(std::hash<std::string>()(path)),
                               uint64_t(blended) | (uint64_t(mode) << 8)};
  if (seen.insert(XXH3_64bits(key_data, sizeof(key_data))).second) {
    REXLOG_INFO(
        "[tess-trace] VS {:016X} PS {:016X} mode {} blended {} -> {} "
        "(patches {}, factors {:.3f}..{:.3f}{}, zfunc {} zwrite {})",
        vs, ps, mode, blended ? 1 : 0, path, patches, factor_min, factor_max,
        bad_factor ? " OUT OF RANGE" : "", zfunc, z_write);
  }
  if (bad_factor) {
    static uint64_t bad = 0;
    if ((++bad & (bad - 1)) == 0) {
      REXLOG_WARN("[tess-trace] {} draws with factors outside [1, 64]", bad);
    }
  }
  ++counts[path];
  if (frame_current_ - window_start >= 600) {
    std::string line;
    for (const auto& [name, n] : counts) {
      line += fmt::format(" {}={:.1f}/frame", name,
                          double(n) / double(frame_current_ - window_start));
    }
    REXLOG_INFO("[tess-trace] last {} frames:{}", frame_current_ - window_start,
                line);
    counts.clear();
    window_start = frame_current_;
  }
}

bool MetalCommandProcessor::ReadAdaptiveTessFactors(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    float factor_min, float factor_max, std::vector<float>& out) {
  out.clear();
  const uint32_t count = primitive_processing_result.host_draw_vertex_count;
  if (primitive_processing_result.tessellation_mode !=
          xenos::TessellationMode::kAdaptive ||
      primitive_processing_result.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kQuadDomainPatchIndexed ||
      primitive_processing_result.index_buffer_type !=
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive_processing_result.host_index_format !=
          xenos::IndexFormat::kInt32 ||
      !count || (count & 3)) {
    return false;
  }
  const auto* words = reinterpret_cast<const uint32_t*>(
      memory_->TranslatePhysical(primitive_processing_result.guest_index_base));
  if (!words) {
    return false;
  }
  const xenos::Endian endian =
      primitive_processing_result.host_shader_index_endian;
  out.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    // tessellation_adaptive_vs: swap per xe_vertex_index_endian, +1, then
    // max(min), min(max).
    uint32_t v = words[i];
    if (endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32) {
      v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    if (endian == xenos::Endian::k8in32 || endian == xenos::Endian::k16in32) {
      v = (v >> 16) | (v << 16);
    }
    float f;
    std::memcpy(&f, &v, sizeof(f));
    f = f + 1.0f;
    // D3D max/min return the non-NaN operand, as fmaxf/fminf do.
    f = std::fmax(f, factor_min);
    f = std::fmin(f, factor_max);
    out[i] = f;
  }
  return true;
}

bool MetalCommandProcessor::DispatchDraw(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool use_tessellation_emulation,
    MetalPipelineCache::TessellationPipelineState* tessellation_pipeline_state,
    bool use_geometry_emulation,
    MetalPipelineCache::GeometryPipelineState* geometry_pipeline_state,
    bool shared_memory_is_uav, MTL::ResourceUsage shared_memory_usage,
    bool memexport_used, MTL::RenderStages memexport_write_stages,
    bool uses_vertex_fetch, bool shared_memory_resource_registered,
    const PreparedIndexBuffer* prepared_guest_dma_index_buffer,
    bool has_vertex_bindings,
    const VertexBindingRange* vertex_ranges, uint32_t vertex_range_count,
    const IndexBufferInfo* index_buffer_info,
    const std::vector<draw_util::MemExportRange>& memexport_ranges) {
  pass_trace::NoteDraw(current_render_pass_trace_);
  auto use_shared_memory_resource_if_needed = [&](MTL::Buffer* buffer) {
    if (!buffer || shared_memory_resource_registered) {
      return;
    }
    UseRenderEncoderResource(buffer, shared_memory_usage);
  };

  // Bind vertex buffers / descriptors.
  if (use_geometry_emulation || use_tessellation_emulation) {
    IRRuntimeVertexBuffers vertex_buffers = {};
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    if (shared_mem_buffer) {
      use_shared_memory_resource_if_needed(shared_mem_buffer);
      for (uint32_t i = 0; i < vertex_range_count; ++i) {
        const auto& range = vertex_ranges[i];
        size_t binding_index = range.binding_index;
        if (binding_index <
            (sizeof(vertex_buffers) / sizeof(vertex_buffers[0]))) {
          vertex_buffers[binding_index].addr =
              shared_mem_buffer->gpuAddress() + range.offset;
          vertex_buffers[binding_index].length = range.length;
          vertex_buffers[binding_index].stride = range.stride;
        }
      }
    }
    // MSC manual: bind IRRuntimeVertexBuffers at kIRVertexBufferBindPoint (6)
    // for the object stage when using geometry emulation.
    current_render_encoder_->setObjectBytes(
        vertex_buffers, sizeof(vertex_buffers), kIRVertexBufferBindPoint);
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kObject,
                                         kIRVertexBufferBindPoint);
  } else if (uses_vertex_fetch) {
    // Vertex fetch shaders read directly from shared memory via SRV, so avoid
    // stage-in bindings that can trigger invalid buffer loads.
    if (shared_memory_) {
      if (MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer()) {
        use_shared_memory_resource_if_needed(shared_mem_buffer);
      }
    }
  } else {
    // Bind vertex buffers at kIRVertexBufferBindPoint (index 6+) for stage-in.
    // The pipeline's vertex descriptor expects buffers at these indices,
    // populated from the vertex fetch constants. The buffer addresses come from
    // shared memory.
    if (shared_memory_ && has_vertex_bindings) {
      MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
      if (shared_mem_buffer) {
        // Mark shared memory as used for reading
        use_shared_memory_resource_if_needed(shared_mem_buffer);

        // Bind vertex buffers for each binding
        for (uint32_t i = 0; i < vertex_range_count; ++i) {
          const auto& range = vertex_ranges[i];
          uint64_t buffer_index =
              kIRVertexBufferBindPoint + uint64_t(range.binding_index);
          SetRenderEncoderVertexBuffer(shared_mem_buffer, range.offset,
                                       buffer_index);
        }
      }
    } else if (shared_memory_) {
      // No vertex bindings, but still mark shared memory as resident
      if (MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer()) {
        use_shared_memory_resource_if_needed(shared_mem_buffer);
      }
    }
  }

  auto validate_guest_index_range = [&](uint64_t index_base,
                                        uint32_t index_count,
                                        MTL::IndexType index_type) -> bool {
    if (!shared_memory_) {
      return false;
    }
    uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint64_t index_length = uint64_t(index_count) * index_stride;
    if (index_base > SharedMemory::kBufferSize ||
        SharedMemory::kBufferSize - index_base < index_length) {
      REXLOG_WARN(
          "Index buffer range out of bounds (base=0x{:08X} size={} count={})",
          static_cast<uint32_t>(index_base), index_length, index_count);
      return false;
    }
    return true;
  };
  auto resolve_guest_dma_index_buffer =
      [&](uint64_t guest_index_base, uint32_t index_count,
          MTL::IndexType index_type, MTL::Buffer*& index_buffer_out,
          uint64_t& index_offset_out) -> bool {
    index_buffer_out = nullptr;
    index_offset_out = 0;
    if (memexport_used) {
      if (!prepared_guest_dma_index_buffer ||
          !prepared_guest_dma_index_buffer->buffer) {
        REXLOG_ERROR(
            "IssueDraw: guest DMA memexport index draw was not prepared for "
            "GPU copy");
        return false;
      }
      index_buffer_out = prepared_guest_dma_index_buffer->buffer;
      index_offset_out = prepared_guest_dma_index_buffer->offset;
      return true;
    }
    if (!validate_guest_index_range(guest_index_base, index_count,
                                    index_type)) {
      return false;
    }
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    if (!shared_mem_buffer) {
      return false;
    }
    index_buffer_out = shared_mem_buffer;
    index_offset_out = guest_index_base;
    return true;
  };

  // Shared index buffer resolution used by tessellation, geometry, and
  // standard indexed draw paths.  Returns false on fatal error.
  auto resolve_index_buffer = [&](MTL::IndexType index_type,
                                  MTL::Buffer*& index_buffer_out,
                                  uint64_t& index_offset_out) -> bool {
    index_buffer_out = nullptr;
    index_offset_out = 0;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (!resolve_guest_dma_index_buffer(
                primitive_processing_result.guest_index_base,
                primitive_processing_result.host_draw_vertex_count, index_type,
                index_buffer_out, index_offset_out)) {
          REXLOG_ERROR("IssueDraw: failed to resolve guest DMA index buffer");
          return false;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        if (primitive_processor_) {
          index_buffer_out = primitive_processor_->GetConvertedIndexBuffer(
              primitive_processing_result.host_index_buffer_handle,
              index_offset_out);
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        if (primitive_processor_) {
          index_buffer_out = primitive_processor_->GetBuiltinIndexBuffer();
          index_offset_out =
              primitive_processing_result.host_index_buffer_handle;
        }
        break;
      default:
        REXLOG_ERROR("Unsupported index buffer type {}",
               uint32_t(primitive_processing_result.index_buffer_type));
        return false;
    }
    if (!index_buffer_out) {
      REXLOG_ERROR("IssueDraw: index buffer is null for type {}",
             uint32_t(primitive_processing_result.index_buffer_type));
      return false;
    }
    UseRenderEncoderResource(index_buffer_out, MTL::ResourceUsageRead);
    return true;
  };

  if (dispatch_native_points_) {
    // FABLE2_NATIVE_POINTS: one 4-vertex strip instance per guest point; the
    // vertex shader reads the guest index itself (XePointDraw at buffer 3).
    dispatch_native_points_ = false;
    struct {
      uint64_t index;
      uint32_t format;
      uint32_t count;
    } point_draw = {};
    const uint32_t instances =
        primitive_processing_result.host_draw_vertex_count /
        std::max(1u, dispatch_native_geometry_vertices_);
    point_draw.count = instances;
    if (primitive_processing_result.index_buffer_type !=
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      point_draw.index = index_buffer->gpuAddress() + index_offset;
      point_draw.format = index_type == MTL::IndexTypeUInt16 ? 1u : 2u;
    }
    if (dispatch_native_points_mesh_) {
      // FABLE2_NATIVE_POINTS=2: the mesh function reads the graphics root
      // arguments itself (64 points per threadgroup).
      dispatch_native_points_mesh_ = false;
      const StageRootArgumentAllocation& root =
          current_bindless_stage_root_arguments_[kStageVertex];
      SetRenderEncoderMeshBuffer(root.buffer, root.offset,
                                 kIRArgumentBufferBindPoint);
      current_render_encoder_->setMeshBytes(&point_draw, sizeof(point_draw), 3);
      InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kMesh, 3);
      current_render_encoder_->drawMeshThreadgroups(
          MTL::Size::Make((instances + 63) / 64, 1, 1),
          MTL::Size::Make(1, 1, 1), MTL::Size::Make(64, 1, 1));
    } else {
      current_render_encoder_->setVertexBytes(&point_draw, sizeof(point_draw),
                                              3);
      InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 3);
      current_render_encoder_->drawPrimitives(
          MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4),
          NS::UInteger(instances));
    }
  } else if (const PreparedDraw* native = dispatch_native_tess_) {
    // FABLE2_NATIVE_TESS: hardware tessellation, factors per patch.
    dispatch_native_tess_ = nullptr;
    struct {
      uint64_t index;
      uint32_t format;
      uint32_t indexed;
    } tess_draw = {};
    if (primitive_processing_result.tessellation_mode !=
        xenos::TessellationMode::kAdaptive) {
      tess_draw.indexed = 1;
      if (primitive_processing_result.index_buffer_type !=
          PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
        MTL::IndexType index_type =
            (primitive_processing_result.host_index_format ==
             xenos::IndexFormat::kInt16)
                ? MTL::IndexTypeUInt16
                : MTL::IndexTypeUInt32;
        MTL::Buffer* index_buffer = nullptr;
        uint64_t index_offset = 0;
        if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
          if (NativeTessTraceEnabled()) {
            REXLOG_WARN("[tess-trace] native draw dropped: index buffer not "
                        "resolved");
          }
          return false;
        }
        tess_draw.index = index_buffer->gpuAddress() + index_offset;
        tess_draw.format = index_type == MTL::IndexTypeUInt16 ? 1u : 2u;
      }
    }
    current_render_encoder_->setVertexBytes(&tess_draw, sizeof(tess_draw), 3);
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 3);
    if (native->native_tess_cpu) {
      // The reference tessellator's patterns: one merged indexed draw, or one
      // instanced draw per run of patches (instance id = patch).
      UseRenderEncoderResource(tess_arena_, MTL::ResourceUsageRead);
      current_render_encoder_->setVertexBuffer(tess_arena_, 0, 4);
      if (native->tess_merged_indices) {
        UseRenderEncoderResource(native->tess_merged_indices,
                                 MTL::ResourceUsageRead);
        current_render_encoder_->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle, NS::UInteger(native->tess_merged_count),
            MTL::IndexTypeUInt32, native->tess_merged_indices,
            NS::UInteger(native->tess_merged_offset));
      }
      for (const TessRun& run : native->tess_runs) {
        UseRenderEncoderResource(run.pattern->indices, MTL::ResourceUsageRead);
        current_render_encoder_->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle, NS::UInteger(run.pattern->index_count),
            MTL::IndexTypeUInt32, run.pattern->indices, 0,
            NS::UInteger(run.count), NS::Integer(run.pattern->arena_base),
            NS::UInteger(run.first));
      }
      InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 4);
      submission_has_draws_ = true;
      return true;
    }
    UseRenderEncoderResource(native->tess_factor_buffer, MTL::ResourceUsageRead);
    current_render_encoder_->setTessellationFactorBuffer(
        native->tess_factor_buffer, native->tess_factor_offset, 0);
    current_render_encoder_->drawPatches(4, 0, native->tess_patch_count,
                                         nullptr, 0, 1, 0);
  } else if (use_tessellation_emulation) {
    IRRuntimePrimitiveType tess_primitive = IRRuntimePrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
        tess_primitive = IRRuntimePrimitiveType3ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kQuadList:
        tess_primitive = IRRuntimePrimitiveType4ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        tess_primitive = (primitive_processing_result.tessellation_mode ==
                          xenos::TessellationMode::kAdaptive)
                             ? IRRuntimePrimitiveType3ControlPointPatchlist
                             : IRRuntimePrimitiveType1ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        tess_primitive = (primitive_processing_result.tessellation_mode ==
                          xenos::TessellationMode::kAdaptive)
                             ? IRRuntimePrimitiveType4ControlPointPatchlist
                             : IRRuntimePrimitiveType1ControlPointPatchlist;
        break;
      default:
        REXLOG_ERROR(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Metal tessellation path",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    if (!tessellation_pipeline_state) {
      return false;  // IssueDrawImpl skips such draws; never dereference
    }
    const IRRuntimeTessellationPipelineConfig& tess_config =
        tessellation_pipeline_state->config;

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPatchesTessellationEmulation(
          current_render_encoder_, tess_primitive, tess_config, 1,
          primitive_processing_result.host_draw_vertex_count, 0, 0);
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint32_t start_index =
          index_stride ? uint32_t(index_offset / index_stride) : 0;
      IRRuntimeDrawIndexedPatchesTessellationEmulation(
          current_render_encoder_, tess_primitive, index_type, index_buffer,
          tess_config, 1, primitive_processing_result.host_draw_vertex_count, 0,
          0, start_index);
    }
  } else if (use_geometry_emulation) {
    IRRuntimePrimitiveType geometry_primitive = IRRuntimePrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_primitive = IRRuntimePrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kRectangleList:
        geometry_primitive = IRRuntimePrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_primitive = IRRuntimePrimitiveTypeLineWithAdj;
        break;
      default:
        REXLOG_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Metal geometry path",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    IRRuntimeGeometryPipelineConfig geometry_config = {};
    geometry_config.gsVertexSizeInBytes =
        geometry_pipeline_state->gs_vertex_size_in_bytes;
    geometry_config.gsMaxInputPrimitivesPerMeshThreadgroup =
        geometry_pipeline_state->gs_max_input_primitives_per_mesh_threadgroup;

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPrimitivesGeometryEmulation(
          current_render_encoder_, geometry_primitive, geometry_config, 1,
          primitive_processing_result.host_draw_vertex_count, 0, 0);
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint32_t start_index =
          index_stride ? uint32_t(index_offset / index_stride) : 0;
      IRRuntimeDrawIndexedPrimitivesGeometryEmulation(
          current_render_encoder_, geometry_primitive, index_type, index_buffer,
          geometry_config, 1,
          primitive_processing_result.host_draw_vertex_count, start_index, 0,
          0);
    }
  } else {
    // Primitive topology - from primitive processor, like D3D12.
    MTL::PrimitiveType mtl_primitive = MTL::PrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        mtl_primitive = MTL::PrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kLineList:
        mtl_primitive = MTL::PrimitiveTypeLine;
        break;
      case xenos::PrimitiveType::kLineStrip:
        mtl_primitive = MTL::PrimitiveTypeLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        mtl_primitive = MTL::PrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        mtl_primitive = MTL::PrimitiveTypeTriangleStrip;
        break;
      default:
        REXLOG_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Metal command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    // Draw using primitive processor output.
    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPrimitives(
          current_render_encoder_, mtl_primitive, NS::UInteger(0),
          NS::UInteger(primitive_processing_result.host_draw_vertex_count));
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      IRRuntimeDrawIndexedPrimitives(
          current_render_encoder_, mtl_primitive,
          NS::UInteger(primitive_processing_result.host_draw_vertex_count),
          index_type, index_buffer, index_offset, NS::UInteger(1), 0, 0);
    }
  }

  if (memexport_used && shared_memory_) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges) {
      if (MetalSharedMemory::MemexportPreserveEnabled()) {
        // Hash the guest bytes before the GPU overwrites the shared copy.
        shared_memory_->NoteMemexportWrite(
            memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      }
      if (MetalSharedMemory::MemexportReadbackEnabled()) {
        // Copied back to guest RAM once this command buffer has completed.
        shared_memory_->NoteMemexportReadback(
            memexport_range.base_address_dwords << 2, memexport_range.size_bytes,
            submission_current_);
      }
      shared_memory_->RangeWrittenByGpu(
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      // [texture-precommit] record the write even when no stage is known
      // (no pending entry is made then): a texture load of these pages must
      // stay behind this draw in the current command buffer.
      shared_memory_->MarkGpuWrite(
          (memexport_range.base_address_dwords << 2) &
              (SharedMemory::kBufferSize - 1),
          memexport_range.size_bytes, submission_current_);
      if (NS::UInteger(memexport_write_stages)) {
        active_render_encoder_shared_memory_write_stages_ = MTL::RenderStages(
            NS::UInteger(active_render_encoder_shared_memory_write_stages_) |
            NS::UInteger(memexport_write_stages));
        MarkSharedMemoryWritePending(memexport_range.base_address_dwords << 2,
                                     memexport_range.size_bytes,
                                     memexport_write_stages, true, false);
      }
    }
  }

  submission_has_draws_ = true;

  return true;
}

  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT: the footprint applies only to
  // 2D fetches with a texel offset along an axis scaled 2x or 3x, and only
  // when the texture's format is not depth-like (k_24_8, k_24_8_FLOAT, k_16,
  // k_16_FLOAT, k_32_FLOAT; the translator's per-fetch rule, before its
  // resolution-scaled and filter tests). If no such fetch of this shader can
  // take it in this draw, its translation without the footprint code samples
  // exactly as the footprint code would.
bool MetalCommandProcessor::FootprintVariantNeeded(const Shader& shader) {
  fp_variant_check_mask_ = 0;
  fp_variant_chosen_ = false;
  if (!EffectsFootprintMode() || !EffectsFootprintVariantMode() ||
      REXCVAR_GET(draw_resolution_scaled_texture_offsets) || !texture_cache_ ||
      !texture_cache_->IsDrawResolutionScaled()) {
    return false;
  }
  const RegisterFile& regs = *register_file_;
  {
    const uint32_t sx = texture_cache_->draw_resolution_scale_x();
    const uint32_t sy = texture_cache_->draw_resolution_scale_y();
    const uint64_t masks =
        static_cast<const MetalShader&>(shader).OffsetFetchConstantMasks();
    uint32_t fetches = ((sx == 2 || sx == 3) ? uint32_t(masks) : 0u) |
                       ((sy == 2 || sy == 3) ? uint32_t(masks >> 32) : 0u);
    uint32_t tf;
    while (rex::bit_scan_forward(fetches, &tf)) {
      fetches &= ~(uint32_t(1) << tf);
      const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(tf);
      switch (xenos::TextureFormat(fetch.dword_1 & 0x3Fu)) {
        case xenos::TextureFormat::k_24_8:
        case xenos::TextureFormat::k_24_8_FLOAT:
        case xenos::TextureFormat::k_16:
        case xenos::TextureFormat::k_16_FLOAT:
        case xenos::TextureFormat::k_32_FLOAT:
          continue;
        default:
          break;
      }
      fp_variant_check_mask_ |= uint32_t(1) << tf;
      // Resolution-scaled: the texture key this draw's request will use.
      if (!texture_cache_->IsFetchConstantScaledResolve(tf, fetch)) {
        continue;
      }
      fp_variant_chosen_ = true;
      ++UploadTrace().fp_variant;
      // Diagnostic: FABLE2_EFFECTS_FOOTPRINT_TRACE=1 logs each distinct
      // (shader, fetch constant, format) that selects the footprint variant.
      static const bool trace = [] {
        const char* v = std::getenv("FABLE2_EFFECTS_FOOTPRINT_TRACE");
        return v && v[0] == '1';
      }();
      if (trace) {
        static std::mutex trace_mutex;
        static std::unordered_set<uint64_t> seen;
        const uint64_t key = shader.ucode_data_hash() ^
                             (uint64_t(tf) << 56) ^
                             (uint64_t(fetch.dword_1 & 0x3Fu) << 48) ^
                             (uint64_t(fetch.dword_2) << 8);
        std::lock_guard<std::mutex> lock(trace_mutex);
        if (seen.insert(key).second) {
          REXLOG_INFO(
              "[effects-footprint] PS {:016X} footprint variant: tf{} format {} "
              "{}x{}",
              shader.ucode_data_hash(), tf, fetch.dword_1 & 0x3Fu,
              (fetch.dword_2 & 0x1FFFu) + 1, ((fetch.dword_2 >> 13) & 0x1FFFu) + 1);
        }
      }
      return true;
    }
  }
  return false;
}


bool MetalCommandProcessor::IssueCopy() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kIssueCopy)) {
    return false;
  }
  TexRenameSettleOrphansNow();
  if (!render_target_cache_) {
    REXLOG_WARN("MetalCommandProcessor::IssueCopy - No render target cache");
    return true;
  }

  MetalRenderTargetCache::ResolvePlan resolve_plan;
  if (!render_target_cache_->PrepareResolvePlan(*memory_, resolve_plan)) {
    REXLOG_ERROR("MetalCommandProcessor::IssueCopy - Resolve planning failed");
    return false;
  }

  uint32_t written_address = 0;
  uint32_t written_length = 0;

  MTL::CommandBuffer* copy_command_buffer = nullptr;
  if (resolve_plan.needs_render_encoder_end) {
    // End only the render encoder here. The resolve/transfer work may still
    // reuse the current Metal command buffer and submission ordering.
    // [tile-resolve] or keep the draw pass open for a tile-function copy;
    // Resolve then ends it at the same point (TileResolveEnd).
    const bool keep_open =
        current_render_encoder_ &&
        (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE] != 0x12704000 &&
        render_target_cache_->TileResolveOffer(resolve_plan);
    if (!keep_open) {
      EndRenderEncoder(RenderEncoderEndReason::kResolveNeedsBoundary);
    }
    copy_command_buffer = EnsureCommandBuffer();
    if (!copy_command_buffer) {
      render_target_cache_->TileResolveEnd("resolve:tile-nocb");
      REXLOG_ERROR("MetalCommandProcessor::IssueCopy: failed to get command buffer");
      return false;
    }
  }

  const bool resolved = render_target_cache_->Resolve(
      *memory_, written_address, written_length, copy_command_buffer,
      &resolve_plan);
  // [tile-resolve] never leave the offered pass open past the resolve.
  render_target_cache_->TileResolveEnd("resolve:tile-exit");
  // [tile-resolve keep] the tile resolve's pass (its clear drawn in it) ends
  // here unless FABLE2_TILE_RESOLVE_KEEP_OPEN=1.
  if (!MetalRenderTargetCache::TileResolveKeepOpenEnabled()) {
    TileResolveEndKept("resolve:tile");
  }
  if (!resolved) {
    REXLOG_ERROR("MetalCommandProcessor::IssueCopy - Resolve failed");
    return false;
  }

  DiagResolveDump(written_address, written_length);
  // Femtofork identifies this destination as the hero/dog morph scratch texture.
  // https://github.com/just-harry/unofficial-xenia-femtofork-for-fable-ii
  // Morph readback currently supports only native resolution. Shared Metal
  // storage is CPU-visible, but is a separate allocation from guest RAM.
  const auto copy_control = register_file_->Get<reg::RB_COPY_CONTROL>();
  const auto copy_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
  if (REXCVAR_GET(metal_fable_morph_readback) && written_length &&
      (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE] == 0x12704000 &&
      copy_control.copy_src_select < xenos::kMaxColorRenderTargets &&
      copy_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 &&
      texture_cache_->IsDrawResolutionScaled() && !resolve_plan.native) {
    // [res-scale] At scale > 1 the resolve wrote only the scaled resolve
    // buffer; downscale it to 1x and copy that to guest RAM.
    if (!MorphReadbackScaled(written_address, written_length, 32)) {
      return false;
    }
  } else if (REXCVAR_GET(metal_fable_morph_readback) && written_length &&
      (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE] == 0x12704000 &&
      copy_control.copy_src_select < xenos::kMaxColorRenderTargets &&
      copy_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 &&
      (!texture_cache_->IsDrawResolutionScaled() || resolve_plan.native)) {
    auto* buffer = shared_memory_->GetBuffer();
    if (!buffer || !buffer->contents() ||
        uint64_t(written_address) + written_length > buffer->length()) {
      REXLOG_ERROR("Metal morph readback: invalid destination extent");
      return false;
    }
    // Resolve already closed its compute encoder.
    if (!WaitForMorphReadbackSubmission(written_address, written_length)) {
      return false;
    }
    auto* destination = memory_->TranslatePhysical(written_address);
    if (!destination) return false;
    const auto* source =
        static_cast<const uint8_t*>(buffer->contents()) + written_address;
    std::memcpy(destination, source, written_length);
    NoteMorphReadback(source, written_address, written_length, false);
  }

  return true;
}

bool MetalCommandProcessor::WaitForMorphReadbackSubmission(
    uint32_t written_address, uint32_t written_length) {
  // Retain the submission so a timeout/error cannot lead to reading data
  // that the GPU is still writing.
  auto* completion = EnsureCommandBuffer();
  if (!completion) return false;
  completion->retain();
  EndRenderEncoder(RenderEncoderEndReason::kResolveNeedsBoundary);
  FlushCommandBufferAndWait(5000000000ULL, "Fable morph readback");
  // A shared-event signal may become visible before Metal publishes the
  // command buffer's terminal status. Do not drop a morph level in that gap.
  const auto completion_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(5);
  auto status = completion->status();
  while (status != MTL::CommandBufferStatusCompleted &&
         status != MTL::CommandBufferStatusError &&
         std::chrono::steady_clock::now() < completion_deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    status = completion->status();
  }
  const bool completed = status == MTL::CommandBufferStatusCompleted;
  completion->release();
  if (!completed) {
    REXLOG_ERROR("Metal morph readback: GPU submission did not complete (status={} address=0x{:08X} bytes={})",
                 uint32_t(status), written_address, written_length);
    return false;
  }
  return true;
}

void MetalCommandProcessor::NoteMorphReadback(const uint8_t* data,
                                              uint32_t written_address,
                                              uint32_t written_length,
                                              bool scaled) {
  static uint32_t morph_readbacks = 0;
  const uint32_t index = ++morph_readbacks;
  // Validation only: FABLE2_MORPH_READBACK_DUMP=<dir> writes the bytes copied
  // to guest RAM (morph-<n>-<bytes>.bin) for a per-pixel diff across scales.
  static const char* dump_dir = std::getenv("FABLE2_MORPH_READBACK_DUMP");
  if (dump_dir && *dump_dir && index <= 64) {
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    const std::filesystem::path path =
        std::filesystem::path(dump_dir) /
        fmt::format("morph-{:03}-{}.bin", index, written_length);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), written_length);
  }
  REXLOG_INFO(
      "Metal morph readback: copied 0x{:08X} bytes={} to guest RAM{} "
      "xxh3={:016X}",
      written_address, written_length,
      scaled ? fmt::format(" (downscaled from {}x{})",
                           texture_cache_->draw_resolution_scale_x(),
                           texture_cache_->draw_resolution_scale_y())
             : std::string(),
      XXH3_64bits(data, written_length));
}

bool MetalCommandProcessor::MorphReadbackScaled(uint32_t written_address,
                                                uint32_t written_length,
                                                uint32_t bits_per_pixel,
                                                const char* diag_dump_path) {
  // Scaled resolve layout (texture_address.xesli): each 1x 32x32 tile becomes
  // scale_x * scale_y group-packed tiles at address * scale_x * scale_y.
  // resolve_downscale (XeniOS, also used by its D3D12/Vulkan readback paths)
  // undoes that layout and keeps the centre sample of each block.
  uint32_t pixel_size_log2 = 2;
  switch (bits_per_pixel) {
    case 8: pixel_size_log2 = 0; break;
    case 16: pixel_size_log2 = 1; break;
    case 32: pixel_size_log2 = 2; break;
    case 64: pixel_size_log2 = 3; break;
    default: return true;
  }
  const uint32_t tile_size_1x = (32u * 32u) << pixel_size_log2;
  const uint32_t tile_count = written_length / tile_size_1x;
  if (!tile_count || (written_length % tile_size_1x) ||
      (written_address % tile_size_1x)) {
    REXLOG_WARN(
        "[res-scale] morph readback: unaligned scaled resolve 0x{:08X} "
        "bytes={}, not copied",
        written_address, written_length);
    return true;
  }
  const uint32_t scale_x = texture_cache_->draw_resolution_scale_x();
  const uint32_t scale_y = texture_cache_->draw_resolution_scale_y();
  const uint64_t scaled_bytes =
      uint64_t(written_length) * scale_x * scale_y;

  if (!morph_downscale_pipeline_ && !morph_downscale_pipeline_failed_) {
    morph_downscale_pipeline_failed_ = true;
    NS::Error* error = nullptr;
    dispatch_data_t data = dispatch_data_create(
        resolve_downscale_cs_metallib, sizeof(resolve_downscale_cs_metallib),
        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* lib = device_->newLibrary(data, &error);
    dispatch_release(data);
    MTL::Function* fn =
        lib ? lib->newFunction(
                  NS::String::string("entry_xe", NS::UTF8StringEncoding))
            : nullptr;
    if (fn) {
      morph_downscale_pipeline_ =
          device_->newComputePipelineState(fn, &error);
      fn->release();
    }
    if (lib) lib->release();
    if (morph_downscale_pipeline_) {
      morph_downscale_pipeline_failed_ = false;
    } else {
      REXLOG_ERROR("[res-scale] morph readback: resolve_downscale pipeline: {}",
                   error ? error->localizedDescription()->utf8String()
                         : "unknown");
    }
  }
  if (!morph_downscale_pipeline_) {
    return true;
  }
  if (!morph_downscale_buffer_ ||
      morph_downscale_buffer_->length() < written_length) {
    if (morph_downscale_buffer_) morph_downscale_buffer_->release();
    morph_downscale_buffer_ = device_->newBuffer(
        std::max<size_t>(written_length, size_t(1) << 20),
        MTL::ResourceStorageModeShared);
    if (!morph_downscale_buffer_) {
      REXLOG_ERROR("[res-scale] morph readback: buffer allocation failed");
      return true;
    }
    morph_downscale_buffer_->setLabel(
        NS::String::string("FableMorphDownscale", NS::UTF8StringEncoding));
  }

  // Align the scaled end to a whole tile: the SDK's scaled range ends at the
  // first scaled byte of the last 1x byte, the shader reads the full tile.
  MTL::Buffer* source = nullptr;
  size_t source_offset = 0;
  size_t source_length = 0;
  texture_cache_->LazyCensusRead(written_address, written_length, true);
  if (!texture_cache_->EnsureScaledResolveMemoryCommitted(written_address,
                                                          written_length, 12) ||
      !texture_cache_->MakeScaledResolveRangeCurrent(written_address,
                                                     written_length, 12) ||
      !texture_cache_->GetCurrentScaledResolveBuffer(source, source_offset,
                                                     source_length) ||
      !source || source_offset + scaled_bytes > source->length()) {
    REXLOG_ERROR(
        "[res-scale] morph readback: no scaled resolve range for 0x{:08X} "
        "bytes={}",
        written_address, written_length);
    return true;
  }

  EndRenderEncoder(RenderEncoderEndReason::kResolveNeedsBoundary);
  MTL::CommandBuffer* cmd = EnsureCommandBuffer();
  if (!cmd) return false;
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kTransferRequest);
  if (!texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    REXLOG_ERROR("[res-scale] morph readback: texture upload flush failed");
  }
  MTL::ComputeCommandEncoder* encoder =
      pass_trace::Compute(cmd, "morph_downscale");
  if (!encoder) return false;
  struct {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t source_offset_bytes;
    uint32_t half_pixel_offset;
  } constants = {scale_x, scale_y, pixel_size_log2, tile_count, 0, 1};
  encoder->setComputePipelineState(morph_downscale_pipeline_);
  encoder->setBytes(&constants, sizeof(constants), 0);
  encoder->setBuffer(source, source_offset, 1);
  encoder->setBuffer(morph_downscale_buffer_, 0, 2);
  encoder->dispatchThreadgroups(MTL::Size::Make(tile_count, 1, 1),
                                MTL::Size::Make(128, 1, 1));
  encoder->endEncoding();

  if (!WaitForMorphReadbackSubmission(written_address, written_length)) {
    return false;
  }
  const auto* data =
      static_cast<const uint8_t*>(morph_downscale_buffer_->contents());
  if (diag_dump_path) {
    std::ofstream out(diag_dump_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), written_length);
    return true;
  }
  auto* destination = memory_->TranslatePhysical(written_address);
  if (!destination) return false;
  std::memcpy(destination, data, written_length);
  NoteMorphReadback(data, written_address, written_length, true);
  return true;
}

void MetalCommandProcessor::DiagResolveDump(uint32_t written_address,
                                            uint32_t written_length) {
  static const char* dir = std::getenv("FABLE2_DIAG_RESOLVE_DUMP");  // getenv-ok: once
  static uint32_t lo = 0, hi = 0;
  static const bool have_range = [] {
    const char* r = std::getenv("FABLE2_DIAG_RESOLVE_RANGE");  // getenv-ok: once
    return r && std::sscanf(r, "%x-%x", &lo, &hi) == 2;
  }();
  if (!dir || !*dir || !have_range || !written_length ||
      written_address > hi || written_address + written_length <= lo) {
    return;
  }
  static uint32_t n = 0;
  if (++n > 512) return;
  const FormatInfo* format_info = FormatInfo::Get(
      uint32_t(register_file_->Get<reg::RB_COPY_DEST_INFO>().copy_dest_format));
  const uint32_t bits_per_pixel = format_info ? format_info->bits_per_pixel : 32;
  {
    const RegisterFile& r = *register_file_;
    REXLOG_INFO(
        "[diag-resolve] #{} frame {} {:08X}+{} copy_ctl {:08X} dest_info "
        "{:08X} dest_pitch {:08X} surf {:08X} color0 {:08X} color1 {:08X} "
        "depth {:08X} win_off {:08X} scr_tl {:08X} scr_br {:08X} "
        "modecntl {:08X} edram_pitch_scale {}",
        n, frame_log::CurrentFrame(), written_address, written_length,
        r[XE_GPU_REG_RB_COPY_CONTROL], r[XE_GPU_REG_RB_COPY_DEST_INFO],
        r[XE_GPU_REG_RB_COPY_DEST_PITCH], r[XE_GPU_REG_RB_SURFACE_INFO],
        r[XE_GPU_REG_RB_COLOR_INFO], r[XE_GPU_REG_RB_COLOR1_INFO],
        r[XE_GPU_REG_RB_DEPTH_INFO], r[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
        r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
        r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], r[XE_GPU_REG_RB_MODECONTROL],
        texture_cache_->draw_resolution_scale_x());
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path =
      (std::filesystem::path(dir) /
       fmt::format("r{:04}-{:08X}-{}.bin", n, written_address, written_length))
          .string();
  if (texture_cache_->IsDrawResolutionScaled()) {
    MorphReadbackScaled(written_address, written_length, bits_per_pixel,
                        path.c_str());
    return;
  }
  auto* buffer = shared_memory_->GetBuffer();
  if (!buffer || !buffer->contents() ||
      uint64_t(written_address) + written_length > buffer->length()) {
    return;
  }
  if (!WaitForMorphReadbackSubmission(written_address, written_length)) return;
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(buffer->contents()) + written_address,
            written_length);
}

void MetalCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void MetalCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

namespace {

bool RegisterRangeContains(uint32_t start, uint32_t end, uint32_t range_start,
                           uint32_t range_end) {
  return start >= range_start && end <= range_end;
}

bool RegisterRangeOverlaps(uint32_t start, uint32_t end, uint32_t range_start,
                           uint32_t range_end) {
  return start < range_end && range_start < end;
}

}  // namespace

void MetalCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (TryWriteKnownRegisterRangeFromMem(start_index, base, num_registers)) {
    return;
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void MetalCommandProcessor::WriteRegisterRangeFromRing(rex::memory::RingBuffer* ring,
                                                       uint32_t base,
                                                       uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (CanFastWriteRegisterRange(base, num_registers)) {
    WriteFastRegisterRangeFromRing(ring, base, num_registers);
    return;
  }
  CommandProcessor::WriteRegisterRangeFromRing(ring, base, num_registers);
}

bool MetalCommandProcessor::CanFastWriteRegisterRange(
    uint32_t start_index, uint32_t num_registers) const {
  if (!num_registers) {
    return true;
  }
  const uint32_t end = start_index + num_registers;
  if (end < start_index || end > RegisterFile::kRegisterCount) {
    return false;
  }
  if (RegisterRangeContains(start_index, end, XE_GPU_REG_SHADER_CONSTANT_000_X,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) ||
      RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1) ||
      RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                            XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1)) {
    return true;
  }

  const bool overlaps_special =
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_SCRATCH_REG0,
                            XE_GPU_REG_SCRATCH_REG7 + 1) ||
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_COHER_STATUS_HOST,
                            XE_GPU_REG_COHER_STATUS_HOST + 1) ||
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_DC_LUT_RW_INDEX,
                            XE_GPU_REG_DC_LUT_30_COLOR + 1);
  const bool overlaps_shader_constants =
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_SHADER_CONSTANT_000_X,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) ||
      RegisterRangeOverlaps(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1) ||
      RegisterRangeOverlaps(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                            XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1);
  return !overlaps_special && !overlaps_shader_constants;
}

bool MetalCommandProcessor::TryWriteKnownRegisterRangeFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (!CanFastWriteRegisterRange(start_index, num_registers)) {
    return false;
  }
  const uint32_t end = start_index + num_registers;
  if (RegisterRangeContains(start_index, end, XE_GPU_REG_SHADER_CONSTANT_000_X,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0)) {
    WriteShaderConstantsFromMem(start_index, base, num_registers);
    return true;
  }
  if (RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1)) {
    WriteFetchConstantsFromMem(start_index, base, num_registers);
    return true;
  }
  if (RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                            XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1)) {
    WriteBoolLoopConstantsFromMem(start_index, base, num_registers);
    return true;
  }

  rex::memory::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
  if (hle_census::TilingTraceActive(frame_log::CurrentFrame())) {
    std::string v;
    for (uint32_t i = 0; i < num_registers && i < 24; ++i) {
      v += fmt::format(" {:04X}={:08X}", start_index + i,
                       register_file_->values[start_index + i]);
    }
    REXLOG_INFO("[tiling-trace] f{} regs{}", frame_log::CurrentFrame(), v);
  }
  return true;
}

void MetalCommandProcessor::WriteFastRegisterRangeFromRing(
    rex::memory::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  rex::memory::RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  if (!range.second) {
    TryWriteKnownRegisterRangeFromMem(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);
    ring->EndRead(range);
    return;
  }

  uint32_t first_registers =
      static_cast<uint32_t>(range.first_length / sizeof(uint32_t));
  TryWriteKnownRegisterRangeFromMem(
      base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      first_registers);
  TryWriteKnownRegisterRangeFromMem(
      base + first_registers,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
      num_registers - first_registers);
  ring->EndRead(range);
}

void MetalCommandProcessor::WriteShaderConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (cbuffer_binding_float_vertex_.up_to_date &&
      FloatConstantRangeNeedsDirty(start_index, base, num_registers,
                                   current_float_constant_map_vertex_, 0)) {
    cbuffer_binding_float_vertex_.up_to_date = false;
  }
  if (cbuffer_binding_float_pixel_.up_to_date &&
      FloatConstantRangeNeedsDirty(start_index, base, num_registers,
                                   current_float_constant_map_pixel_, 256)) {
    cbuffer_binding_float_pixel_.up_to_date = false;
  }
  rex::memory::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
}

void MetalCommandProcessor::WriteBoolLoopConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  rex::memory::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
  cbuffer_binding_bool_loop_.up_to_date = false;
}

void MetalCommandProcessor::WriteFetchConstantsFromMem(uint32_t start_index,
                                                       uint32_t* base,
                                                       uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  DxbcShader::FetchConstantDwordMask written_fetch_dword_mask = {};
  const uint32_t dword_start =
      start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
  const uint32_t dword_end = dword_start + num_registers;
  for (uint32_t dword = dword_start; dword < dword_end; ++dword) {
    MarkFetchConstantDword(written_fetch_dword_mask, dword);
  }

  // [draw-prep-lite] texture fetch constant slots whose dwords actually change:
  // a slot rewritten with the same values keeps its texture binding in sync (a
  // re-parse would produce the same key, signedness and swizzle).
  const bool lite = texture_cache_ && DrawPrepLiteEnabled();
  uint32_t changed_slots = 0;
  if (lite) {
    const uint32_t* old_values = &register_file_->values[start_index];
    for (uint32_t i = 0; i < num_registers; ++i) {
      if (rex::memory::load_and_swap<uint32_t>(base + i) != old_values[i]) {
        changed_slots |= UINT32_C(1) << ((dword_start + i) / 6);
      }
    }
  }

  rex::memory::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);

  DirtyFetchConstantDwords(written_fetch_dword_mask);
  if (texture_cache_) {
    if (lite) {
      uint32_t slot;
      while (rex::bit_scan_forward(changed_slots, &slot)) {
        changed_slots = rex::clear_lowest_bit(changed_slots);
        texture_cache_->TextureFetchConstantWritten(slot);
      }
    } else {
      texture_cache_->TextureFetchConstantsWritten(dword_start / 6,
                                                   (dword_end - 1) / 6);
    }
  }
}

bool MetalCommandProcessor::FloatConstantRangeNeedsDirty(
    uint32_t start_index, const uint32_t* base, uint32_t num_registers,
    const uint64_t* constant_map, uint32_t stage_first_constant) const {
  constexpr uint32_t kStageFloatConstantCount = 256;
  const uint32_t written_dword_start =
      start_index - XE_GPU_REG_SHADER_CONSTANT_000_X;
  const uint32_t written_dword_end = written_dword_start + num_registers;
  const uint32_t stage_dword_start = stage_first_constant * 4;
  const uint32_t stage_dword_end =
      (stage_first_constant + kStageFloatConstantCount) * 4;
  uint32_t dword = std::max(written_dword_start, stage_dword_start);
  const uint32_t dword_end = std::min(written_dword_end, stage_dword_end);
  const bool dirty_on_change = REXCVAR_GET(metal_float_constants_dirty_on_change);

  while (dword < dword_end) {
    const uint32_t relative_constant = (dword - stage_dword_start) >> 2;
    const uint64_t live_constant = constant_map[relative_constant >> 6] &
                                   (uint64_t(1) << (relative_constant & 63));
    const uint32_t constant_dword_end =
        std::min(dword_end, stage_dword_start + ((relative_constant + 1) << 2));
    if (!live_constant) {
      dword = constant_dword_end;
      continue;
    }
    if (!dirty_on_change) {
      return true;
    }
    for (; dword < constant_dword_end; ++dword) {
      const uint32_t value =
          rex::memory::load_and_swap<uint32_t>(base + (dword - written_dword_start));
      if (register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X + dword] !=
          value) {
        return true;
      }
    }
  }
  return false;
}

void MetalCommandProcessor::DirtyFetchConstantDwords(
    const DxbcShader::FetchConstantDwordMask& dirty_mask) {
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    MergeFetchConstantDwordMask(fetch_constant_dirty_masks_[stage], dirty_mask);
    if (FetchConstantDwordMasksOverlap(
            dirty_mask, current_fetch_constant_dword_masks_[stage])) {
      cbuffer_binding_fetch_[stage].up_to_date = false;
    }
  }
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  uint32_t old_value = 0;
  bool valid_register = index < RegisterFile::kRegisterCount;
  if (valid_register) {
    old_value = register_file_->values[index];
  }
  CommandProcessor::WriteRegister(index, value);
  if (!valid_register) {
    return;
  }
  if (hle_census_hooks_ && (index < XE_GPU_REG_SHADER_CONSTANT_000_X ||
                            index > XE_GPU_REG_SHADER_CONSTANT_LOOP_31) &&
      hle_census::TilingTraceActive(frame_log::CurrentFrame())) {
    REXLOG_INFO("[tiling-trace] f{} reg {:04X}={:08X}", frame_log::CurrentFrame(), index,
                value);
  }
  bool value_changed = old_value != value;

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (REXCVAR_GET(metal_float_constants_dirty_on_change) && !value_changed) {
      return;
    }
    uint32_t float_constant_index =
        (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (float_constant_index >= 256) {
      uint32_t rel = float_constant_index - 256;
      if (current_float_constant_map_pixel_[rel >> 6] &
          (uint64_t(1) << (rel & 63))) {
        cbuffer_binding_float_pixel_.up_to_date = false;
      }
    } else {
      if (current_float_constant_map_vertex_[float_constant_index >> 6] &
          (uint64_t(1) << (float_constant_index & 63))) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    const uint32_t fetch_dword = index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    DxbcShader::FetchConstantDwordMask changed_fetch_dword_mask = {};
    MarkFetchConstantDword(changed_fetch_dword_mask, fetch_dword);
    DirtyFetchConstantDwords(changed_fetch_dword_mask);
    // [draw-prep-lite] an unchanged value keeps the texture binding in sync.
    if (texture_cache_ && (value_changed || !DrawPrepLiteEnabled())) {
      texture_cache_->TextureFetchConstantWritten(fetch_dword / 6);
    }
  }
}

MTL::CommandBuffer* MetalCommandProcessor::EnsureCommandBuffer() {
  ProcessCompletedSubmissions();
  if (current_command_buffer_) {
    return current_command_buffer_;
  }
  if (!command_queue_) {
    REXLOG_ERROR("EnsureCommandBuffer: no command queue");
    return nullptr;
  }
  const bool is_opening_frame = !frame_open_;
  if (is_opening_frame) {
    OpenFrameLifetime();
  }

  EnsureCommandBufferAutoreleasePool();

  // Note: commandBuffer() returns an autoreleased object, we must retain it.
  {
    ScopedAutoreleasePool autorelease_pool;
    current_command_buffer_ = command_queue_->commandBuffer();
    if (current_command_buffer_) {
      current_command_buffer_->retain();
      if (EncoderDebugLabelsEnabled()) {
        current_command_buffer_->setLabel(
            NS::String::string("XeniaCommandBuffer", NS::UTF8StringEncoding));
      }
    }
  }
  gpu_time_trace::AddHandler(current_command_buffer_, "gpu_cb");
  if (!current_command_buffer_) {
    REXLOG_ERROR("EnsureCommandBuffer: failed to create command buffer");
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }

  ++submission_current_;
  cb_sampled_textures_.clear();
  // [upload-precommit] a new command buffer starts after pre-commit copies
  // that may still be running (RAW for pages they made valid).
  // Any pre-commit command buffer is committed by now (with the previous
  // main command buffer).
  EncodePrecommitWaitCommitted(current_command_buffer_);
  precommit_waited_value_ = precommit_event_value_;

  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  current_command_buffer_->addCompletedHandler(
      [this, zpd_submission = submission_current_](
          MTL::CommandBuffer* completed_cmd) {
        if (zpd_real_) {
          // [occlusion-real] write the results of queries that finished here.
          ZpdOnSubmissionCompleted(
              zpd_submission,
              completed_cmd->status() == MTL::CommandBufferStatusError);
        }
        if (completed_cmd->status() == MTL::CommandBufferStatusError) {
          NS::Error* error = completed_cmd->error();
          if (error) {
            REXLOG_ERROR("Metal command buffer error: {}",
                   error->localizedDescription()->utf8String());
          }
        }
        {
          std::lock_guard<std::mutex> lock(completion_mutex_);
          completed_command_buffers_.fetch_add(1, std::memory_order_release);
          pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
        }
        completion_cond_.notify_all();
      });

  if (texture_cache_) {
    texture_cache_->BeginSubmission(submission_current_);
  }
  submission_has_draws_ = false;
  if (is_opening_frame) {
    if (primitive_processor_) {
      primitive_processor_->BeginFrame();
    }
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
    }
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
    frame_open_ = true;
  }

  return current_command_buffer_;
}

void MetalCommandProcessor::ProcessCompletedSubmissions() {
  const uint64_t completed =
      completed_command_buffers_.load(std::memory_order_relaxed);
  if (completed <= submission_completed_processed_) {
    return;
  }
  submission_completed_processed_ = completed;
  if (texture_cache_) {
    texture_cache_->CompletedSubmissionUpdated(completed);
  }
  if (shared_memory_ && MetalSharedMemory::UploadVerifyEnabled()) {
    // Before the readback, which may legitimately change guest RAM.
    shared_memory_->VerifyCompletedUploads(completed);
  }
  if (shared_memory_ && MetalSharedMemory::MemexportReadbackEnabled()) {
    shared_memory_->ApplyCompletedMemexportReadbacks(completed);
  }
  PruneCompletedSharedMemoryWrites(completed);
  while (!retired_view_bindless_indices_.empty() &&
         retired_view_bindless_indices_.front().submission_id <= completed) {
    FreeViewBindlessIndexNow(retired_view_bindless_indices_.front().index);
    retired_view_bindless_indices_.pop_front();
  }
  while (!retired_sampler_bindless_indices_.empty() &&
         retired_sampler_bindless_indices_.front().submission_id <= completed) {
    FreeSamplerBindlessIndexNow(
        retired_sampler_bindless_indices_.front().index);
    retired_sampler_bindless_indices_.pop_front();
  }
  while (!retired_memexport_index_buffers_.empty() &&
         retired_memexport_index_buffers_.front().submission_id <= completed) {
    if (retired_memexport_index_buffers_.front().buffer) {
      retired_memexport_index_buffers_.front().buffer->release();
    }
    retired_memexport_index_buffers_.pop_front();
  }
}

void MetalCommandProcessor::WaitForFrameSlotSubmission(
    uint64_t awaited_submission) {
  if (!awaited_submission) {
    return;
  }
  if (completed_command_buffers_.load(std::memory_order_acquire) >=
      awaited_submission) {
    return;
  }
  ++backend_telemetry_.frame_slot_waits;
  ++backend_telemetry_.frame_slot_wait_submission_count;
  backend_telemetry_.frame_slot_wait_submission_last = awaited_submission;
  const uint64_t trace_wait_start = gpu_time_trace::Now();
  hitch::Scope hitch_scope(hitch::kSlot);
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completion_cond_.wait(lock, [&]() {
      return completed_command_buffers_.load(std::memory_order_acquire) >=
             awaited_submission;
    });
  }
  gpu_time_trace::RecordWait("tr_slot_wait_us", trace_wait_start);
}

void MetalCommandProcessor::OpenFrameLifetime() {
  const uint64_t awaited_submission =
      closed_frame_submissions_[frame_current_ % kMaxFramesInFlight];
  WaitForFrameSlotSubmission(awaited_submission);
  ProcessCompletedSubmissions();

  const uint64_t completed_submission =
      completed_command_buffers_.load(std::memory_order_acquire);
  frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) -
                     kMaxFramesInFlight;
  for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
    if (closed_frame_submissions_[frame % kMaxFramesInFlight] >
        completed_submission) {
      break;
    }
    frame_completed_ = frame;
  }

  if (constant_buffer_pool_) {
    constant_buffer_pool_->Reclaim(frame_completed_);
  }
  InvalidateFrameTransientBindings();
}

void MetalCommandProcessor::InvalidateFrameTransientBindings() {
  cbuffer_binding_system_.up_to_date = false;
  cbuffer_binding_float_vertex_.up_to_date = false;
  cbuffer_binding_float_pixel_.up_to_date = false;
  cbuffer_binding_bool_loop_.up_to_date = false;
  for (auto& fetch_binding : cbuffer_binding_fetch_) {
    fetch_binding.up_to_date = false;
  }
  cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
  cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
  std::memset(current_float_constant_map_vertex_, 0,
              sizeof(current_float_constant_map_vertex_));
  std::memset(current_float_constant_map_pixel_, 0,
              sizeof(current_float_constant_map_pixel_));
  current_payload_system_.clear();
  current_payload_float_vertex_.clear();
  current_payload_float_pixel_.clear();
  current_payload_bool_loop_.clear();
  for (auto& payload : current_payload_fetch_) {
    payload.clear();
  }
  for (auto& payload : scratch_payload_fetch_) {
    payload.clear();
  }
  constant_payload_cache_.clear();
  constant_payload_cache_bytes_ = 0;

  current_bindless_stage_root_valid_.fill(false);
  current_bindless_stage_root_frame_open_rebuild_pending_.fill(true);
  current_bindless_stage_root_serials_.fill(0);
  current_bindless_cbv_gpu_addresses_ = {};
  current_bindless_cbv_buffers_ = {};
  current_bindless_cbv_offsets_ = {};
  current_bindless_active_cbv_masks_.fill(0);
  current_bindless_root_resource_set_ = {};
  current_bindless_root_resource_source_serial_ = 0;
  render_encoder_bindless_root_resources_serial_ = 0;
  current_bindless_stage_root_arguments_ = {};
  current_bindless_stage_root_entries_ = {};
  current_bindless_stage_root_entries_valid_.fill(false);
  current_bindless_shared_memory_is_uav_ = false;
}

void MetalCommandProcessor::CloseFrameLifetime() {
  if (!frame_open_) {
    return;
  }
  if (primitive_processor_) {
    primitive_processor_->EndFrame();
  }
  frame_open_ = false;

  const uint32_t frame_slot = uint32_t(frame_current_ % kMaxFramesInFlight);
  const uint64_t previous_slot_submission =
      closed_frame_submissions_[frame_slot];
  assert_true(!previous_slot_submission ||
              previous_slot_submission <=
                  completed_command_buffers_.load(std::memory_order_acquire));
  closed_frame_submissions_[frame_slot] = submission_current_;
  ++frame_current_;
}

void MetalCommandProcessor::MaybeDumpBackendTelemetry(const char* reason,
                                                      bool force) {
  if (!REXCVAR_GET(metal_backend_telemetry)) {
    return;
  }
  int32_t interval_config = REXCVAR_GET(metal_backend_telemetry_interval);
  uint64_t interval = interval_config > 0 ? uint64_t(interval_config) : 0;
  if (!force &&
      (!interval || backend_telemetry_.swaps <
                        backend_telemetry_last_dump_swap_ + interval)) {
    return;
  }

  MetalRenderTargetCache::TelemetryStats rt_stats = {};
  if (render_target_cache_) {
    rt_stats = render_target_cache_->GetAndResetTelemetryStats();
  }

  const std::string end_reasons = MetalFormatNamedCounts(
      backend_telemetry_.end_reasons, MetalRenderEncoderEndReasonName);
  const std::string transfer_request_sources = MetalFormatNamedTriplets(
      backend_telemetry_.transfer_request_sources_total,
      backend_telemetry_.transfer_request_sources_active,
      backend_telemetry_.transfer_request_sources_no_active,
      MetalTransferRequestSourceName);
  const std::string transfer_request_render_end_sources =
      MetalFormatNamedCounts(
          backend_telemetry_.transfer_request_render_encoder_ends,
          MetalTransferRequestSourceName);
  const std::string shared_memory_request_upload_bytes = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_request_upload_bytes,
      MetalSharedMemoryRequestReasonName);
  const std::string shared_memory_request_failures =
      MetalFormatNamedCounts(backend_telemetry_.shared_memory_request_failures,
                             MetalSharedMemoryRequestReasonName);
  const std::string shared_memory_request_outcomes =
      MetalFormatNamedCounts(backend_telemetry_.shared_memory_request_outcomes,
                             MetalSharedMemoryRequestOutcomeName);
  const std::string shared_memory_upload_route_counts = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_upload_route_counts,
      MetalSharedMemoryUploadRouteName);
  const std::string shared_memory_upload_route_bytes = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_upload_route_bytes,
      MetalSharedMemoryUploadRouteName);
  const std::string shared_memory_direct_write_rejects = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_direct_write_reject_counts,
      MetalSharedMemoryDirectWriteRejectReasonName);
  const std::string shared_memory_direct_write_reject_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.shared_memory_direct_write_reject_bytes,
          MetalSharedMemoryDirectWriteRejectReasonName);
  const std::string texture_upload_source_route_counts = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_source_route_counts,
      MetalTextureUploadSourceRouteName);
  const std::string texture_upload_source_route_bytes = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_source_route_bytes,
      MetalTextureUploadSourceRouteName);
  const std::string texture_upload_source_fallback_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.texture_upload_source_fallback_reasons,
          MetalTextureUploadSourceFallbackReasonName);
  const std::string shared_memory_upload_encoder_end_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.shared_memory_upload_encoder_end_reasons,
          MetalSharedMemoryUploadEncoderEndReasonName);
  const std::string draw_materialization_source_ranges = MetalFormatNamedCounts(
      backend_telemetry_.draw_materialization_source_ranges,
      MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_bytes = MetalFormatNamedCounts(
      backend_telemetry_.draw_materialization_source_bytes,
      MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_invalid_ranges =
      MetalFormatNamedCounts(
          backend_telemetry_.draw_materialization_source_invalid_ranges,
          MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_invalid_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.draw_materialization_source_invalid_bytes,
          MetalTelemetryDrawMaterializationSourceName);
  const std::string prepared_draw_queue_flush_reasons = MetalFormatNamedCounts(
      backend_telemetry_.prepared_draw_queue_flush_reasons,
      MetalPreparedDrawFlushReasonName);
  const std::string prepared_draw_queue_reject_reasons = MetalFormatNamedCounts(
      backend_telemetry_.prepared_draw_queue_reject_reasons,
      MetalPreparedDrawQueueRejectReasonName);
  const std::string cbv_uploads = MetalFormatNamedCounts(
      backend_telemetry_.cbv_uploads, MetalTelemetryCbvSlotName);
  const std::string cbv_reuse_hits = MetalFormatNamedCounts(
      backend_telemetry_.cbv_reuse_hits, MetalTelemetryCbvSlotName);
  const std::string descriptor_index_uploads =
      MetalFormatNamedCounts(backend_telemetry_.descriptor_index_uploads,
                             MetalTelemetryShaderStageName);
  const std::string constant_payload_cache_hits =
      MetalFormatNamedCounts(backend_telemetry_.constant_payload_cache_hits,
                             MetalTelemetryCbvSlotName);
  const std::string constant_payload_cache_misses =
      MetalFormatNamedCounts(backend_telemetry_.constant_payload_cache_misses,
                             MetalTelemetryCbvSlotName);
  const std::string constant_payload_cache_bytes_saved = MetalFormatNamedCounts(
      backend_telemetry_.constant_payload_cache_bytes_saved,
      MetalTelemetryCbvSlotName);
  const std::string descriptor_index_payload_cache_hits =
      MetalFormatNamedCounts(
          backend_telemetry_.descriptor_index_payload_cache_hits,
          MetalTelemetryShaderStageName);
  const std::string descriptor_index_payload_cache_misses =
      MetalFormatNamedCounts(
          backend_telemetry_.descriptor_index_payload_cache_misses,
          MetalTelemetryShaderStageName);
  const std::string descriptor_index_payload_cache_bytes_saved =
      MetalFormatNamedCounts(
          backend_telemetry_.descriptor_index_payload_cache_bytes_saved,
          MetalTelemetryShaderStageName);
  const std::string bindless_root_allocations =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_allocations,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_reuse_hits = MetalFormatNamedCounts(
      backend_telemetry_.bindless_root_reuse_hits, MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_noop_updates =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_noop_updates,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_slots_patched =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_slots_patched,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_bytes_copied =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_bytes_copied,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_slot_patches =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_slot_patches,
                             MetalTelemetryRootArgSlotName);
  const std::string bindless_root_rebuild_reasons =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_rebuild_reasons,
                             MetalTelemetryRootRebuildReasonName);
  const std::string encoder_buffer_full_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_full_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_offset_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_offset_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_noop_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_noop_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_null_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_null_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_untracked_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_untracked_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string render_resource_set_applies =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_applies,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_set_skips =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_skips,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_set_resources =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_resources,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_serial_skips =
      MetalFormatNamedCounts(
          backend_telemetry_.render_resource_registry_serial_skips,
          MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_builds =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_registry_builds,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_registers = MetalFormatNamedCounts(
      backend_telemetry_.render_resource_registry_registers,
      MetalTelemetryRenderResourceSetName);
  const auto& direct_host_stats = rt_stats.resolve_direct_host;
  MetalStageCompileCacheStats stage_compile_stats = {};
  MetalPipelineRuntimeStats pipeline_runtime_stats = {};
  if (pipeline_cache_) {
    stage_compile_stats = pipeline_cache_->GetAndResetStageCompileStats();
    pipeline_runtime_stats = pipeline_cache_->GetAndResetRuntimeStats();
  }

  REXLOG_INFO(
      "MetalTelemetry[{}]: work swaps={} draws={} pipelines set/skip={}/{} "
      "texture_requests before/after_encoder={}/{}",
      reason, backend_telemetry_.swaps - backend_telemetry_last_dump_swap_,
      backend_telemetry_.draw_calls, backend_telemetry_.pipeline_sets,
      backend_telemetry_.pipeline_set_skips,
      backend_telemetry_.texture_requests_before_encoder,
      backend_telemetry_.texture_requests_after_encoder_begin);
  REXLOG_INFO(
      "MetalTelemetry[{}]: render_encoder begin_calls={} reused={} created={} "
      "descriptor_restarts={} resource_resets={} desc_fail={} create_fail={} "
      "end active/no_active={}/{} reasons={{ {} }}",
      reason, backend_telemetry_.begin_encoder_calls,
      backend_telemetry_.begin_encoder_reused_compatible,
      backend_telemetry_.begin_encoder_created,
      backend_telemetry_.begin_encoder_descriptor_restarts,
      backend_telemetry_.begin_encoder_resource_usage_resets,
      backend_telemetry_.begin_encoder_descriptor_failures,
      backend_telemetry_.begin_encoder_creation_failures,
      backend_telemetry_.end_encoder_active,
      backend_telemetry_.end_encoder_no_active, end_reasons);
  REXLOG_INFO(
      "MetalTelemetry[{}]: transfer_request sources "
      "total/active/no_active={{ {} }} render_end={{ {} }}",
      reason, transfer_request_sources, transfer_request_render_end_sources);
  REXLOG_INFO("MetalTelemetry[{}]: shared_memory request failures={{ {} }}", reason,
         shared_memory_request_failures);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory upload bytes={{ {} }} "
      "outcomes={{ {} }}",
      reason, shared_memory_request_upload_bytes,
      shared_memory_request_outcomes);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory upload_batches requests={} "
      "ranges input/coalesced={}/{} bytes={} upload_encoder "
      "acquire/reuse/copies={}/{}/{} closes={{ {} }} routes counts={{ {} }} "
      "bytes={{ {} }}",
      reason, backend_telemetry_.shared_memory_upload_batches,
      backend_telemetry_.shared_memory_upload_batch_input_ranges,
      backend_telemetry_.shared_memory_upload_batch_coalesced_ranges,
      backend_telemetry_.shared_memory_upload_batch_bytes,
      backend_telemetry_.shared_memory_upload_encoder_acquisitions,
      backend_telemetry_.shared_memory_upload_encoder_reuses,
      backend_telemetry_.shared_memory_upload_encoder_copies,
      shared_memory_upload_encoder_end_reasons,
      shared_memory_upload_route_counts, shared_memory_upload_route_bytes);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory direct_write "
      "eligible/staged_bytes={}/{} rejects={{ {} }} reject_bytes={{ {} }}",
      reason, backend_telemetry_.shared_memory_direct_write_eligible_bytes,
      backend_telemetry_.shared_memory_direct_write_staged_required_bytes,
      shared_memory_direct_write_rejects,
      shared_memory_direct_write_reject_bytes);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory lazy_upload batches "
      "no_upload/direct_only/mixed/staged_only={}/{}/{}/{} "
      "render_active direct_only/mixed/staged_only={}/{}/{}",
      reason, backend_telemetry_.shared_memory_lazy_upload_no_upload_batches,
      backend_telemetry_.shared_memory_lazy_upload_direct_only_batches,
      backend_telemetry_.shared_memory_lazy_upload_mixed_batches,
      backend_telemetry_.shared_memory_lazy_upload_staged_only_batches,
      backend_telemetry_.shared_memory_lazy_upload_direct_only_active,
      backend_telemetry_.shared_memory_lazy_upload_mixed_active,
      backend_telemetry_.shared_memory_lazy_upload_staged_only_active);
  REXLOG_INFO(
      "MetalTelemetry[{}]: texture_upload_source routes counts={{ {} }} "
      "bytes={{ {} }} fallback_reasons={{ {} }}",
      reason, texture_upload_source_route_counts,
      texture_upload_source_route_bytes,
      texture_upload_source_fallback_reasons);
  REXLOG_INFO(
      "MetalTelemetry[{}]: draw_materialization ranges={{ {} }} bytes={{ {} }} "
      "invalid_ranges={{ {} }} invalid_bytes={{ {} }} "
      "per_draw requests/invalid/resident_skip={}/{}/{}",
      reason, draw_materialization_source_ranges,
      draw_materialization_source_bytes,
      draw_materialization_source_invalid_ranges,
      draw_materialization_source_invalid_bytes,
      backend_telemetry_.draw_materialization_per_draw_requests,
      backend_telemetry_.draw_materialization_per_draw_invalid_requests,
      backend_telemetry_.draw_materialization_per_draw_resident_skips);
  REXLOG_INFO(
      "MetalTelemetry[{}]: prepared_draw_queue appends={} flushes={} "
      "single_draw_flushes={} draws_flushed={} ranges_flushed={} "
      "bytes_flushed={} invalid_flushes={} texture_plans={} "
      "texture_loads planned/executed={}/{} flush_reasons={{ {} }} "
      "rejects={{ {} }}",
      reason, backend_telemetry_.prepared_draw_queue_appends,
      backend_telemetry_.prepared_draw_queue_flushes,
      backend_telemetry_.prepared_draw_queue_single_draw_flushes,
      backend_telemetry_.prepared_draw_queue_draws_flushed,
      backend_telemetry_.prepared_draw_queue_ranges_flushed,
      backend_telemetry_.prepared_draw_queue_bytes_flushed,
      backend_telemetry_.prepared_draw_queue_invalid_flushes,
      backend_telemetry_.prepared_draw_queue_texture_plans_flushed,
      backend_telemetry_.prepared_draw_queue_texture_loads_planned,
      backend_telemetry_.prepared_draw_queue_texture_loads_executed,
      prepared_draw_queue_flush_reasons, prepared_draw_queue_reject_reasons);
  REXLOG_INFO(
      "MetalTelemetry[{}]: constants cbv_uploads={{ {} }} "
      "cbv_reuse={{ {} }} descriptor_index_uploads={{ {} }} "
      "root_allocations={{ {} }} root_reuse={{ {} }} "
      "root_rebuild_reasons={{ {} }} frame_slot_waits={} "
      "waited_submissions={} last_wait_submission={}",
      reason, cbv_uploads, cbv_reuse_hits, descriptor_index_uploads,
      bindless_root_allocations, bindless_root_reuse_hits,
      bindless_root_rebuild_reasons, backend_telemetry_.frame_slot_waits,
      backend_telemetry_.frame_slot_wait_submission_count,
      backend_telemetry_.frame_slot_wait_submission_last);
  REXLOG_INFO(
      "MetalTelemetry[{}]: binding_cache constant_payload hits={{ {} }} "
      "misses={{ {} }} bytes_saved={{ {} }} descriptor_payload hits={{ {} }} "
      "misses={{ {} }} bytes_saved={{ {} }}",
      reason, constant_payload_cache_hits, constant_payload_cache_misses,
      constant_payload_cache_bytes_saved, descriptor_index_payload_cache_hits,
      descriptor_index_payload_cache_misses,
      descriptor_index_payload_cache_bytes_saved);
  REXLOG_INFO(
      "MetalTelemetry[{}]: root_args allocs={{ {} }} reuse={{ {} }} "
      "noop={{ {} }} slots_patched={{ {} }} bytes_copied={{ {} }} "
      "slot_patches={{ {} }} rebuild_reasons={{ {} }}",
      reason, bindless_root_allocations, bindless_root_reuse_hits,
      bindless_root_arg_noop_updates, bindless_root_arg_slots_patched,
      bindless_root_arg_bytes_copied, bindless_root_arg_slot_patches,
      bindless_root_rebuild_reasons);
  if (REXCVAR_GET(metal_root_rebuild_detail_telemetry)) {
    const std::string bindless_root_slots_changed =
        MetalFormatNamedCounts(backend_telemetry_.bindless_root_slots_changed,
                               MetalTelemetryRootSlotsChangedName);
    const std::string bindless_root_rebuild_details =
        MetalFormatNamedCounts(backend_telemetry_.bindless_root_rebuild_details,
                               MetalTelemetryRootRebuildDetailName);
    REXLOG_INFO(
        "MetalTelemetry[{}]: root_rebuild_detail slots_changed={{ {} }} "
        "details={{ {} }}",
        reason, bindless_root_slots_changed, bindless_root_rebuild_details);
  }
  REXLOG_INFO(
      "MetalTelemetry[{}]: encoder_bindings full={{ {} }} offset={{ {} }} "
      "noop={{ {} }} null={{ {} }} untracked={{ {} }} resource_use "
      "calls/skips/upgrades={}/{}/{} batches/resources/skips={}/{}/{} "
      "resource_sets apply={{ {} }} skip={{ {} }} resources={{ {} }}",
      reason, encoder_buffer_full_binds, encoder_buffer_offset_binds,
      encoder_buffer_noop_binds, encoder_buffer_null_binds,
      encoder_buffer_untracked_binds,
      backend_telemetry_.render_encoder_use_resource_calls,
      backend_telemetry_.render_encoder_use_resource_skips,
      backend_telemetry_.render_encoder_use_resource_upgrades,
      backend_telemetry_.render_encoder_use_resources_batches,
      backend_telemetry_.render_encoder_use_resources_requested,
      backend_telemetry_.render_encoder_use_resources_skips,
      render_resource_set_applies, render_resource_set_skips,
      render_resource_set_resources);
  REXLOG_INFO(
      "MetalTelemetry[{}]: resource_registry serial_skip={{ {} }} "
      "build={{ {} }} register={{ {} }}",
      reason, render_resource_registry_serial_skips,
      render_resource_registry_builds, render_resource_registry_registers);
  REXLOG_INFO(
      "MetalTelemetry[{}]: residency_set supported/enabled/attached={}/{}/{} "
      "allocations added/duplicates/commits={}/{}/{} "
      "resource_refs covered/fallback={}/{} "
      "use_resource covered/fallback={}/{} "
      "use_heap covered/fallback={}/{} live_allocations={}",
      reason, residency_set_supported_ ? 1 : 0, residency_set_enabled_ ? 1 : 0,
      residency_set_attached_ ? 1 : 0,
      backend_telemetry_.residency_set_allocations_added,
      backend_telemetry_.residency_set_allocation_duplicates,
      backend_telemetry_.residency_set_commits,
      backend_telemetry_.residency_set_resource_refs_covered,
      backend_telemetry_.residency_set_resource_refs_fallback,
      backend_telemetry_.residency_set_use_resources_covered,
      backend_telemetry_.residency_set_use_resources_fallback,
      backend_telemetry_.residency_set_use_heaps_covered,
      backend_telemetry_.residency_set_use_heaps_fallback,
      residency_set_ ? uint64_t(residency_set_->allocationCount()) : 0);
  REXLOG_INFO(
      "MetalTelemetry[{}]: resolve_direct_host attempt/success={}/{} "
      "reject gamma/exp_bias/format/sample/depth_no_fast={}/{}/{}/{}/{}",
      reason, direct_host_stats.direct_host_attempt,
      direct_host_stats.direct_host_success,
      direct_host_stats.direct_host_reject_gamma,
      direct_host_stats.direct_host_reject_exp_bias,
      direct_host_stats.direct_host_reject_format_mismatch,
      direct_host_stats.direct_host_reject_sample_select,
      direct_host_stats.direct_host_reject_depth_no_fast);
  REXLOG_INFO(
      "MetalTelemetry[{}]: stage_compile requests={} hits/misses={}/{} "
      "waits={} failures={} persistent hits/misses={}/{} "
      "bytes dxil/metallib={}/{} owner_ms total/max={}/{} "
      "wait_ms total/max={}/{}",
      reason, stage_compile_stats.requests, stage_compile_stats.memory_hits,
      stage_compile_stats.memory_misses, stage_compile_stats.waits,
      stage_compile_stats.failures, stage_compile_stats.persistent_hits,
      stage_compile_stats.persistent_misses, stage_compile_stats.dxil_bytes,
      stage_compile_stats.metallib_bytes,
      stage_compile_stats.owner_compile_ms_total,
      stage_compile_stats.owner_compile_ms_max,
      stage_compile_stats.wait_ms_total, stage_compile_stats.wait_ms_max);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shader_prep dxil requests/hits/misses/failures="
      "{}/{}/{}/{} "
      "bytes dxbc/dxil={}/{} ms total/max={}/{} libraries "
      "requests/failures={}/{} bytes={} ms total/max={}/{} render_pso "
      "requests/failures={}/{} ms total/max={}/{}",
      reason, pipeline_runtime_stats.dxil_convert_requests,
      pipeline_runtime_stats.dxil_cache_hits,
      pipeline_runtime_stats.dxil_cache_misses,
      pipeline_runtime_stats.dxil_convert_failures,
      pipeline_runtime_stats.dxil_convert_dxbc_bytes,
      pipeline_runtime_stats.dxil_convert_dxil_bytes,
      pipeline_runtime_stats.dxil_convert_ms_total,
      pipeline_runtime_stats.dxil_convert_ms_max,
      pipeline_runtime_stats.library_requests,
      pipeline_runtime_stats.library_failures,
      pipeline_runtime_stats.library_bytes,
      pipeline_runtime_stats.library_ms_total,
      pipeline_runtime_stats.library_ms_max,
      pipeline_runtime_stats.render_pipeline_requests,
      pipeline_runtime_stats.render_pipeline_failures,
      pipeline_runtime_stats.render_pipeline_ms_total,
      pipeline_runtime_stats.render_pipeline_ms_max);
  ResetBackendTelemetry();
}

void MetalCommandProcessor::ResetBackendTelemetry() {
  backend_telemetry_last_dump_swap_ = backend_telemetry_.swaps;
  backend_telemetry_ = BackendTelemetryStats();
  backend_telemetry_.swaps = backend_telemetry_last_dump_swap_;
}

void MetalCommandProcessor::EnsureCommandBufferAutoreleasePool() {
  if (command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_ = NS::AutoreleasePool::alloc()->init();
}

void MetalCommandProcessor::DrainCommandBufferAutoreleasePool() {
  if (!command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_->release();
  command_buffer_autorelease_pool_ = nullptr;
}

void MetalCommandProcessor::EndRenderEncoder() {
  EndRenderEncoder(RenderEncoderEndReason::kUnknown);
}

void MetalCommandProcessor::EndRenderEncoderForResolve(const char* detail) {
  if (current_render_encoder_ && detail) {
    render_end_detail_ = detail;
  }
  EndRenderEncoder(RenderEncoderEndReason::kResolveNeedsBoundary);
}

void MetalCommandProcessor::EndRenderEncoder(RenderEncoderEndReason reason) {
  if (!flushing_prepared_draw_queue_ && !prepared_draw_queue_.empty()) {
    if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kRenderEncoderEnd)) {
      REXLOG_ERROR("Metal EndRenderEncoder: failed to flush prepared draw queue");
    }
  }
  size_t reason_index = static_cast<size_t>(reason);
  if (reason_index < backend_telemetry_.end_reasons.size()) {
    ++backend_telemetry_.end_reasons[reason_index];
  }
  const char* end_detail = render_end_detail_;
  render_end_detail_ = nullptr;
  if (!current_render_encoder_) {
    ++backend_telemetry_.end_encoder_no_active;
    current_render_encoder_has_zpd_visibility_ = false;
    if (current_render_pass_descriptor_) {
      current_render_pass_descriptor_->release();
      current_render_pass_descriptor_ = nullptr;
    }
    ResetRenderEncoderBufferBindings();
    ResetRenderEncoderResourceUsage();
    return;
  }
  ++backend_telemetry_.end_encoder_active;
  // Close any active ZPD query segment before ending the encoder.
  // Metal visibility results are scoped to the render encoder; the offset
  // cannot be reused after the encoder is ended.

  UpdateSharedMemoryFenceForActiveRenderEncoder();
  pass_trace::EndPass(current_render_pass_trace_,
                      end_detail ? end_detail
                                 : MetalRenderEncoderEndReasonName(reason_index));
  current_render_pass_trace_ = {};
  PassExtentRecordEnd();
  ResetRenderDependencyDedup();
  current_render_encoder_->endEncoding();
  current_render_encoder_->release();
  current_render_encoder_ = nullptr;
  ++render_encoder_gen_;
  if (texture_cache_) {
    texture_cache_->TileResolveNoteEncoderEnd();
  }
  // [upload-precommit] later encoders of this command buffer start after
  // any pre-commit copy (RAW); no-op unless one was committed.
  EncodePrecommitWait(current_command_buffer_);
  current_render_encoder_has_zpd_visibility_ = false;
  ResetRenderEncoderBufferBindings();
  ResetRenderEncoderResourceUsage();
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  current_render_pipeline_state_ = nullptr;
  rasterizer_state_valid_ = false;
  current_depth_stencil_state_ = nullptr;
  stencil_reference_valid_ = false;
  heap_binds_set_on_encoder_ = false;
  ResetRenderEncoderBufferBindings();
}

void MetalCommandProcessor::NoteDrawPassTransferInheritedRasterizerState() {
  // [raster-state] diagnostic: rasterizer_state_valid_ is true only after a
  // guest draw applied its state in this encoder (a new encoder or a reset
  // leaves Metal's defaults, see ResetDrawPassRasterizerState).
  UploadTraceCounters& u = UploadTrace();
  ++u.xq_inpass;
  if (!rasterizer_state_valid_) {
    return;
  }
  ++u.xq_reused;
  bool cull = current_cull_mode_ != MTL::CullModeNone;
  bool fill = current_triangle_fill_mode_ != MTL::TriangleFillModeFill;
  bool bias = current_depth_bias_values_[0] != 0.0f ||
              current_depth_bias_values_[1] != 0.0f ||
              current_depth_bias_values_[2] != 0.0f;
  bool clip = current_depth_clip_mode_ != MTL::DepthClipModeClip;
  u.xq_inh_cull += cull;
  u.xq_inh_fill += fill;
  u.xq_inh_bias += bias;
  u.xq_inh_clip += clip;
  static uint32_t logged = 0;
  if ((cull || fill || bias || clip) && logged < 16) {
    ++logged;
    REXLOG_INFO(
        "[raster-state] draw-pass transfers inherit cull={} winding={} "
        "fill={} bias={}/{} clip={} (reset {})",
        uint32_t(current_cull_mode_), uint32_t(current_front_facing_winding_),
        uint32_t(current_triangle_fill_mode_), current_depth_bias_values_[0],
        current_depth_bias_values_[1], uint32_t(current_depth_clip_mode_),
        MetalRenderTargetCache::TransferQuadStateResetEnabled() ? "on"
                                                                : "off");
  }
}

void MetalCommandProcessor::InvalidateRenderEncoderStateAfterDrawPassTransfers(
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations) {
  if (!mutations) {
    return;
  }
  using RTC = MetalRenderTargetCache;
  if (mutations & RTC::kDrawPassTransferEncoderMutationPipeline) {
    current_render_pipeline_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationDepthStencil) {
    current_depth_stencil_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationStencilReference) {
    stencil_reference_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationViewport) {
    viewport_dirty_ = true;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationScissor) {
    scissor_dirty_ = true;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationRasterizer) {
    rasterizer_state_valid_ = false;
  }

  constexpr RTC::DrawPassTransferEncoderMutationMask kTransferBufferMutations =
      RTC::kDrawPassTransferEncoderMutationVertexSlot0 |
      RTC::kDrawPassTransferEncoderMutationVertexSlot1 |
      RTC::kDrawPassTransferEncoderMutationFragmentSlot0 |
      RTC::kDrawPassTransferEncoderMutationFragmentSlot1;
  if (mutations & RTC::kDrawPassTransferEncoderMutationVertexSlot0) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 0);
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationVertexSlot1) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 1);
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationFragmentSlot0) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kFragment,
                                         0);
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationFragmentSlot1) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kFragment,
                                         1);
  }
  if (mutations & kTransferBufferMutations) {
    heap_binds_set_on_encoder_ = false;
  }
}

bool MetalCommandProcessor::RequestSharedMemoryRange(
    SharedMemoryRequestReason reason, uint32_t start, uint32_t length) {
  SharedMemory::Range range = {start, length};
  return RequestSharedMemoryRanges(reason, &range, 1);
}

bool MetalCommandProcessor::RequestSharedMemoryRangeBeforeDrawPass(
    SharedMemoryRequestReason reason, uint32_t start, uint32_t length) {
  SharedMemory::Range range = {start, length};
  PrepareSharedMemoryUploadBeforeDrawPass(&range, 1);
  return RequestSharedMemoryRanges(reason, &range, 1);
}

bool MetalCommandProcessor::RequestSharedMemoryRanges(
    SharedMemoryRequestReason reason, const SharedMemory::Range* ranges,
    uint32_t range_count) {
  const bool was_active = current_render_encoder_ != nullptr;
  const size_t reason_index = static_cast<size_t>(reason);
  const bool reason_valid = reason_index < kSharedMemoryRequestReasonCount;
  if (!shared_memory_) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kNoSharedMemory);
    if (reason_valid) {
      ++backend_telemetry_.shared_memory_request_failures[reason_index];
    }
    return false;
  }
  if (range_count && !ranges) {
    if (reason_valid) {
      ++backend_telemetry_.shared_memory_request_failures[reason_index];
    }
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kRequestFailed);
    return false;
  }

  std::vector<SharedMemory::Range> coalesced_ranges;
  const SharedMemory::Range* request_ranges = ranges;
  uint32_t request_range_count = range_count;
  if (ranges && range_count) {
    coalesced_ranges.reserve(range_count);
    for (uint32_t i = 0; i < range_count; ++i) {
      const SharedMemory::Range& range = ranges[i];
      if (!range.length) {
        continue;
      }
      coalesced_ranges.push_back(range);
    }
    if (!coalesced_ranges.empty()) {
      std::sort(coalesced_ranges.begin(), coalesced_ranges.end(),
                [](const SharedMemory::Range& a, const SharedMemory::Range& b) {
                  return a.start < b.start;
                });
      size_t coalesced_count = 0;
      for (SharedMemory::Range range : coalesced_ranges) {
        uint64_t range_end = uint64_t(range.start) + range.length;
        if (range_end > SharedMemory::kBufferSize) {
          coalesced_ranges[coalesced_count++] = range;
          continue;
        }
        if (!coalesced_count) {
          coalesced_ranges[coalesced_count++] = range;
          continue;
        }
        SharedMemory::Range& previous = coalesced_ranges[coalesced_count - 1];
        uint64_t previous_end = uint64_t(previous.start) + previous.length;
        if (range.start <= previous_end) {
          uint64_t merged_end = std::max(previous_end, range_end);
          previous.length = static_cast<uint32_t>(merged_end - previous.start);
        } else {
          coalesced_ranges[coalesced_count++] = range;
        }
      }
      coalesced_ranges.resize(coalesced_count);
    }
    request_ranges = coalesced_ranges.data();
    request_range_count = static_cast<uint32_t>(coalesced_ranges.size());
    if (request_range_count) {
      ++backend_telemetry_.shared_memory_upload_batches;
      backend_telemetry_.shared_memory_upload_batch_input_ranges += range_count;
      backend_telemetry_.shared_memory_upload_batch_coalesced_ranges +=
          request_range_count;
    }
  }

  SharedMemory::RequestRangeStats stats;
  SharedMemoryRequestReason previous_upload_reason =
      current_shared_memory_upload_reason_;
  current_shared_memory_upload_reason_ = reason;
  const bool success = shared_memory_->RequestRanges(
      request_ranges, request_range_count, &stats);
  current_shared_memory_upload_reason_ = previous_upload_reason;
  backend_telemetry_.shared_memory_upload_batch_bytes += stats.upload_bytes;
  if (reason_valid) {
    backend_telemetry_.shared_memory_request_upload_bytes[reason_index] +=
        stats.upload_bytes;
    if (!success) {
      ++backend_telemetry_.shared_memory_request_failures[reason_index];
    }
  }

  if (!success) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kRequestFailed);
  } else if (!stats.upload_bytes) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kAlreadyResident);
  } else if (was_active) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kUploadInsideRenderEncoder);
  } else {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kUploadBeforeRenderEncoder);
  }

  return success;
}

void MetalCommandProcessor::RecordSharedMemoryRequestOutcome(
    SharedMemoryRequestOutcome outcome) {
  const size_t outcome_index = static_cast<size_t>(outcome);
  if (outcome_index < kSharedMemoryRequestOutcomeCount) {
    ++backend_telemetry_.shared_memory_request_outcomes[outcome_index];
  }
}

bool MetalCommandProcessor::AnySharedMemoryRangeInvalid(
    const SharedMemory::Range* ranges, uint32_t range_count) const {
  if (!shared_memory_ || !ranges || !range_count) {
    return false;
  }
  for (uint32_t i = 0; i < range_count; ++i) {
    if (!shared_memory_->IsRangeValid(ranges[i].start, ranges[i].length)) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::RecordSharedMemoryLazyUploadRoute(
    const MetalSharedMemory::UploadRouteInfo& route_info,
    bool render_encoder_active) {
  if (!route_info.upload_bytes) {
    ++backend_telemetry_.shared_memory_lazy_upload_no_upload_batches;
    return;
  }
  if (route_info.direct_bytes && !route_info.staged_bytes) {
    ++backend_telemetry_.shared_memory_lazy_upload_direct_only_batches;
    if (render_encoder_active) {
      ++backend_telemetry_.shared_memory_lazy_upload_direct_only_active;
    }
    return;
  }
  if (route_info.direct_bytes && route_info.staged_bytes) {
    ++backend_telemetry_.shared_memory_lazy_upload_mixed_batches;
    if (render_encoder_active) {
      ++backend_telemetry_.shared_memory_lazy_upload_mixed_active;
    }
    return;
  }
  ++backend_telemetry_.shared_memory_lazy_upload_staged_only_batches;
  if (render_encoder_active) {
    ++backend_telemetry_.shared_memory_lazy_upload_staged_only_active;
  }
}

void MetalCommandProcessor::PrepareSharedMemoryUploadBeforeDrawPass(
    const SharedMemory::Range* ranges, uint32_t range_count) {
  if (!shared_memory_ || !ranges || !range_count) {
    return;
  }
  const bool render_encoder_active = current_render_encoder_ != nullptr;
  MetalSharedMemory::UploadRouteInfo route_info =
      shared_memory_->GetUploadRouteInfo(ranges, range_count);
  RecordSharedMemoryLazyUploadRoute(route_info, render_encoder_active);
  if (render_encoder_active && route_info.inpass_staged_bytes) {
    CountUploadBreak(
        route_info.pages_current      ? kUploadBreakCurrent
        : route_info.pages_standalone ? kUploadBreakStandalone
        : route_info.pages_inflight   ? (SharedMemoryPrecommitAllowed()
                                             ? kUploadBreakOther
                                             : kUploadBreakGuard)
                                      : kUploadBreakOther,
        true);
    EndRenderEncoder(RenderEncoderEndReason::kSharedMemoryUploadBeforeDrawPass);
  }
}

bool MetalCommandProcessor::HasActiveSharedMemoryWritePending() const {
  if (NS::UInteger(active_render_encoder_shared_memory_write_stages_)) {
    return true;
  }
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (pending.active_render_encoder) {
      return true;
    }
  }
  return false;
}

MTL::CommandBuffer* MetalCommandProcessor::RequestTransferCommandBuffer(
    TransferRequestSource source) {
  if (!flushing_prepared_draw_queue_ && !prepared_draw_queue_.empty()) {
    if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kTransferRequest)) {
      return nullptr;
    }
  }
  const size_t source_index = static_cast<size_t>(source);
  const bool ends_render_encoder = current_render_encoder_ != nullptr;
  if (source_index < kTransferRequestSourceCount) {
    ++backend_telemetry_.transfer_request_sources_total[source_index];
    if (ends_render_encoder) {
      ++backend_telemetry_.transfer_request_sources_active[source_index];
      ++backend_telemetry_.transfer_request_render_encoder_ends[source_index];
    } else {
      ++backend_telemetry_.transfer_request_sources_no_active[source_index];
    }
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kTransferRequest);
  if (current_render_encoder_) {
    if (source == TransferRequestSource::kSharedMemoryUpload) {
      CountUploadBreak(pending_upload_break_cause_, false);
    } else {
      render_end_detail_ =
          source == TransferRequestSource::kGuestIndexCopy
              ? "transfer:index_copy"
              : source == TransferRequestSource::kRenderTargetTransfer
                    ? "transfer:rt_transfer"
                    : "transfer:unknown";
    }
    EndRenderEncoder(RenderEncoderEndReason::kRequestTransferCommandBuffer);
  }
  pending_upload_break_cause_ = 0;
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    return nullptr;
  }
  return EnsureCommandBuffer();
}

void MetalCommandProcessor::RecordSharedMemoryUploadRoute(
    SharedMemoryUploadRoute route, uint64_t bytes) {
  const size_t route_index = static_cast<size_t>(route);
  if (route_index >= kSharedMemoryUploadRouteCount) {
    return;
  }
  ++backend_telemetry_.shared_memory_upload_route_counts[route_index];
  backend_telemetry_.shared_memory_upload_route_bytes[route_index] += bytes;
}

void MetalCommandProcessor::RecordSharedMemoryDirectWriteEligibility(
    uint64_t direct_bytes, uint64_t staged_bytes) {
  backend_telemetry_.shared_memory_direct_write_eligible_bytes += direct_bytes;
  backend_telemetry_.shared_memory_direct_write_staged_required_bytes +=
      staged_bytes;
}

void MetalCommandProcessor::RecordSharedMemoryDirectWriteReject(
    SharedMemoryDirectWriteRejectReason reason, uint64_t bytes) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index >= kSharedMemoryDirectWriteRejectReasonCount) {
    return;
  }
  ++backend_telemetry_.shared_memory_direct_write_reject_counts[reason_index];
  backend_telemetry_.shared_memory_direct_write_reject_bytes[reason_index] +=
      bytes;
}

void MetalCommandProcessor::RecordTextureUploadSourceRoute(
    TextureUploadSourceRoute route, uint64_t bytes) {
  const size_t route_index = static_cast<size_t>(route);
  if (route_index >= kTextureUploadSourceRouteCount) {
    return;
  }
  ++backend_telemetry_.texture_upload_source_route_counts[route_index];
  backend_telemetry_.texture_upload_source_route_bytes[route_index] += bytes;
}

void MetalCommandProcessor::RecordTextureUploadSourceFallback(
    TextureUploadSourceFallbackReason reason) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index < kTextureUploadSourceFallbackReasonCount) {
    ++backend_telemetry_.texture_upload_source_fallback_reasons[reason_index];
  }
}

void MetalCommandProcessor::RecordSharedMemoryUploadEncoderCopy() {
  ++backend_telemetry_.shared_memory_upload_encoder_copies;
}

MTL::BlitCommandEncoder*
MetalCommandProcessor::GetSharedMemoryUploadBlitEncoder() {
  if (shared_memory_upload_blit_encoder_) {
    ++backend_telemetry_.shared_memory_upload_encoder_reuses;
    return shared_memory_upload_blit_encoder_;
  }

  MTL::CommandBuffer* command_buffer =
      RequestTransferCommandBuffer(TransferRequestSource::kSharedMemoryUpload);
  if (!command_buffer) {
    return nullptr;
  }

  shared_memory_upload_blit_encoder_ = pass_trace::Blit(command_buffer, "shmem_upload");
  if (!shared_memory_upload_blit_encoder_) {
    REXLOG_ERROR("Metal: failed to create shared-memory upload blit encoder");
    return nullptr;
  }
  ++backend_telemetry_.shared_memory_upload_encoder_acquisitions;
  shared_memory_upload_blit_encoder_->retain();
  if (EncoderDebugLabelsEnabled()) {
    shared_memory_upload_blit_encoder_->setLabel(
        NS::String::string("XeniaSharedMemoryUpload", NS::UTF8StringEncoding));
  }
  return shared_memory_upload_blit_encoder_;
}

void MetalCommandProcessor::EndSharedMemoryUploadBlitEncoder(
    SharedMemoryUploadEncoderEndReason reason) {
  if (!shared_memory_upload_blit_encoder_) {
    return;
  }
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index < kSharedMemoryUploadEncoderEndReasonCount) {
    ++backend_telemetry_.shared_memory_upload_encoder_end_reasons[reason_index];
  }
  shared_memory_upload_blit_encoder_->endEncoding();
  shared_memory_upload_blit_encoder_->release();
  shared_memory_upload_blit_encoder_ = nullptr;
}

MTL::CommandBuffer*
MetalCommandProcessor::CreateStandaloneTransferCommandBuffer(
    const char* label) {
  if (!command_queue_) {
    return nullptr;
  }
  MTL::CommandBuffer* cmd = nullptr;
  {
    ScopedAutoreleasePool autorelease_pool;
    cmd = command_queue_->commandBuffer();
    if (cmd) {
      cmd->retain();
      if (label && EncoderDebugLabelsEnabled()) {
        cmd->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
      }
    }
  }
  if (!cmd) {
    return nullptr;
  }
  // [upload-precommit] standalone work (texture uploads from shared memory,
  // and later pre-commit copies) starts after pre-commit copies still running.
  EncodePrecommitWaitCommitted(cmd);
  gpu_time_trace::AddHandler(cmd, "gpu_cb_sa");
  return cmd;
}

void MetalCommandProcessor::CommitStandaloneAsync(MTL::CommandBuffer* cmd) {
  if (!cmd) {
    return;
  }
  cmd->addCompletedHandler(^(MTL::CommandBuffer* completed_cmd) {
    completed_cmd->release();
  });
  cmd->commit();
}

void MetalCommandProcessor::CommitStandaloneAndWait(MTL::CommandBuffer* cmd) {
  if (!cmd) {
    return;
  }
  cmd->commit();
  const uint64_t trace_wait_start = gpu_time_trace::Now();
  hitch::Scope hitch_scope(hitch::kSync);
  cmd->waitUntilCompleted();
  gpu_time_trace::RecordWait("tr_sync_wait_us", trace_wait_start);
  cmd->release();
}

void MetalCommandProcessor::ResetRenderEncoderResourceUsage() {
  for (EncoderResourceUsageTableEntry& entry :
       render_encoder_resource_usage_table_) {
    entry = {};
  }
  render_encoder_resource_usage_count_ = 0;
  render_encoder_heap_usage_.clear();
  render_encoder_bindless_fixed_resources_serial_ = 0;
  render_encoder_bindless_texture_resources_serial_ = 0;
  render_encoder_bindless_root_resources_serial_ = 0;
}

void MetalCommandProcessor::AddRenderResourceRef(RenderResourceSet& set,
                                                 MTL::Resource* resource,
                                                 MTL::ResourceUsage usage,
                                                 MTL::RenderStages stages) {
  if (!resource) {
    return;
  }
  if (IsResidencySetResourceCovered(resource)) {
    ++backend_telemetry_.residency_set_resource_refs_covered;
    return;
  }
  ++backend_telemetry_.residency_set_resource_refs_fallback;
  uint32_t stage_bits = MetalRenderStageBits(stages);
  if (!stage_bits) {
    stages = MetalAllGraphicsRenderStages();
    stage_bits = MetalRenderStageBits(stages);
  }
  const uint32_t usage_bits = MetalResourceUsageBits(usage);
  for (RenderResourceRef& existing : set.resources) {
    if (existing.resource != resource) {
      continue;
    }
    existing.usage =
        MTL::ResourceUsage(MetalResourceUsageBits(existing.usage) | usage_bits);
    existing.stages =
        MTL::RenderStages(MetalRenderStageBits(existing.stages) | stage_bits);
    return;
  }
  set.resources.push_back({resource, usage, stages});
}

void MetalCommandProcessor::RestoreRenderResourceSet(
    RenderResourceSetKind kind, RenderResourceSet& current,
    const RenderResourceSet& snapshot) {
  const size_t kind_index = static_cast<size_t>(kind);
  if (snapshot.source_serial &&
      current.source_serial == snapshot.source_serial) {
    if (kind_index <
        backend_telemetry_.render_resource_registry_serial_skips.size()) {
      ++backend_telemetry_.render_resource_registry_serial_skips[kind_index];
    }
    return;
  }

  bool same = current.resources.size() == snapshot.resources.size();
  if (same) {
    for (size_t i = 0; i < current.resources.size(); ++i) {
      const RenderResourceRef& current_ref = current.resources[i];
      const RenderResourceRef& snapshot_ref = snapshot.resources[i];
      if (current_ref.resource != snapshot_ref.resource ||
          MetalResourceUsageBits(current_ref.usage) !=
              MetalResourceUsageBits(snapshot_ref.usage) ||
          MetalRenderStageBits(current_ref.stages) !=
              MetalRenderStageBits(snapshot_ref.stages)) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    current.source_serial = snapshot.source_serial;
    return;
  }

  if (kind_index < backend_telemetry_.render_resource_registry_builds.size()) {
    ++backend_telemetry_.render_resource_registry_builds[kind_index];
  }
  uint64_t next_serial = current.serial + 1;
  if (!next_serial) {
    next_serial = 1;
  }
  current.resources = snapshot.resources;
  current.serial = next_serial;
  current.source_serial = snapshot.source_serial;
}

void MetalCommandProcessor::PublishRenderResourceSet(RenderResourceSet& current,
                                                     RenderResourceSet&& next) {
  if (next.source_serial && current.source_serial == next.source_serial) {
    return;
  }
  bool same = current.resources.size() == next.resources.size();
  if (same) {
    for (size_t i = 0; i < current.resources.size(); ++i) {
      const RenderResourceRef& current_ref = current.resources[i];
      const RenderResourceRef& next_ref = next.resources[i];
      if (current_ref.resource != next_ref.resource ||
          MetalResourceUsageBits(current_ref.usage) !=
              MetalResourceUsageBits(next_ref.usage) ||
          MetalRenderStageBits(current_ref.stages) !=
              MetalRenderStageBits(next_ref.stages)) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    current.source_serial = next.source_serial;
    return;
  }
  uint64_t next_serial = current.serial + 1;
  if (!next_serial) {
    next_serial = 1;
  }
  // Swap (not move): with FABLE2_PREPARED_DRAW_POOL `next` is the reused scratch set, which
  // keeps the old storage for the next build. Same contents either way.
  current.resources.swap(next.resources);
  current.serial = next_serial;
  current.source_serial = next.source_serial;
}

uint64_t MetalCommandProcessor::GetBindlessFixedResourceSourceSerial(
    MTL::ResourceUsage shared_memory_usage) const {
  const uint64_t shared_memory_usage_bits =
      uint64_t(MetalResourceUsageBits(shared_memory_usage));
  const uint64_t rt_serial =
      render_target_cache_ ? render_target_cache_->GetBindlessResourcesSerial()
                           : 0;
  return (rt_serial << 8) ^ shared_memory_usage_bits;
}

uint64_t MetalCommandProcessor::GetBindlessTextureResourceSourceSerial() const {
  uint64_t hash = 1469598103934665603ull;
  auto hash_value = [&](uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  hash_value(current_texture_bindless_resources_vertex_.size());
  for (MTL::Texture* texture : current_texture_bindless_resources_vertex_) {
    hash_value(reinterpret_cast<uintptr_t>(texture));
  }
  hash_value(0x9e3779b97f4a7c15ull);
  hash_value(current_texture_bindless_resources_pixel_.size());
  for (MTL::Texture* texture : current_texture_bindless_resources_pixel_) {
    hash_value(reinterpret_cast<uintptr_t>(texture));
  }
  return hash ? hash : 1;
}

uint64_t MetalCommandProcessor::GetBindlessRootResourceSourceSerial(
    const UniformBufferInfo& uniforms) const {
  uint64_t hash = 1469598103934665603ull;
  auto hash_value = [&](uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  constexpr size_t kGraphicsRootStage = kStageVertex;
  const StageRootArgumentAllocation& allocation =
      current_bindless_stage_root_arguments_[kGraphicsRootStage];
  hash_value(reinterpret_cast<uintptr_t>(allocation.valid ? allocation.buffer
                                                          : nullptr));
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    hash_value(uniforms.active_cbv_masks[stage]);
    for (size_t cbv = 0; cbv < kCbvSlotCount; ++cbv) {
      const bool active =
          uniforms.active_cbv_masks[stage] & (uint32_t(1) << cbv);
      hash_value(reinterpret_cast<uintptr_t>(
          active ? uniforms.cbvs[stage][cbv].buffer : nullptr));
    }
  }
  return hash ? hash : 1;
}

void MetalCommandProcessor::PublishBindlessFixedResourceSet(
    MTL::ResourceUsage shared_memory_usage) {
  constexpr size_t kFixedKindIndex = size_t(RenderResourceSetKind::kFixed);
  const uint64_t source_serial =
      GetBindlessFixedResourceSourceSerial(shared_memory_usage);
  if (source_serial &&
      current_bindless_fixed_resource_source_serial_ == source_serial) {
    ++backend_telemetry_.render_resource_registry_serial_skips[kFixedKindIndex];
    return;
  }
  ++backend_telemetry_.render_resource_registry_builds[kFixedKindIndex];
  RenderResourceSet next_storage;
  RenderResourceSet& next = AcquireResourceSetScratch(next_storage);
  next.source_serial = source_serial;
  const MTL::RenderStages stages = MetalAllGraphicsRenderStages();
  AddRenderResourceRef(next,
                       shared_memory_ ? shared_memory_->GetBuffer() : nullptr,
                       shared_memory_usage, stages);
  std::vector<MTL::Resource*> render_target_resources;
  if (render_target_cache_) {
    render_target_cache_->CollectBindlessResources(render_target_resources);
  }
  for (MTL::Resource* resource : render_target_resources) {
    AddRenderResourceRef(next, resource,
                         MTL::ResourceUsageRead | MTL::ResourceUsageWrite,
                         stages);
  }
  AddRenderResourceRef(next, null_buffer_, MTL::ResourceUsageRead, stages);
  AddRenderResourceRef(next, view_bindless_heap_, MTL::ResourceUsageRead,
                       stages);
  AddRenderResourceRef(next, sampler_bindless_heap_, MTL::ResourceUsageRead,
                       stages);
  AddRenderResourceRef(next, system_view_tables_, MTL::ResourceUsageRead,
                       stages);
  PublishRenderResourceSet(current_bindless_fixed_resource_set_,
                           std::move(next));
  current_bindless_fixed_resource_source_serial_ =
      current_bindless_fixed_resource_set_.source_serial;
}

void MetalCommandProcessor::RestoreBindlessTextureResourceSet(
    const RenderResourceSet& set) {
  RestoreRenderResourceSet(RenderResourceSetKind::kTexture,
                           current_bindless_texture_resource_set_, set);
}

void MetalCommandProcessor::PublishBindlessTextureResourceSet() {
  constexpr size_t kTextureKindIndex = size_t(RenderResourceSetKind::kTexture);
  const uint64_t source_serial = GetBindlessTextureResourceSourceSerial();
  if (source_serial &&
      current_bindless_texture_resource_source_serial_ == source_serial) {
    ++backend_telemetry_
          .render_resource_registry_serial_skips[kTextureKindIndex];
    return;
  }
  ++backend_telemetry_.render_resource_registry_builds[kTextureKindIndex];
  RenderResourceSet next_storage;
  RenderResourceSet& next = AcquireResourceSetScratch(next_storage);
  next.source_serial = source_serial;
  const MTL::RenderStages stages = MetalAllGraphicsRenderStages();
  for (MTL::Texture* texture : current_texture_bindless_resources_vertex_) {
    AddRenderResourceRef(next, texture, MTL::ResourceUsageRead, stages);
  }
  for (MTL::Texture* texture : current_texture_bindless_resources_pixel_) {
    AddRenderResourceRef(next, texture, MTL::ResourceUsageRead, stages);
  }
  PublishRenderResourceSet(current_bindless_texture_resource_set_,
                           std::move(next));
  current_bindless_texture_resource_source_serial_ =
      current_bindless_texture_resource_set_.source_serial;
}

void MetalCommandProcessor::PublishBindlessRootResourceSet(
    const UniformBufferInfo& uniforms) {
  constexpr size_t kRootKindIndex = size_t(RenderResourceSetKind::kRoot);
  const uint64_t root_source_serial =
      GetBindlessRootResourceSourceSerial(uniforms);
  if (root_source_serial &&
      current_bindless_root_resource_source_serial_ == root_source_serial) {
    ++backend_telemetry_.render_resource_registry_serial_skips[kRootKindIndex];
    return;
  }
  ++backend_telemetry_.render_resource_registry_builds[kRootKindIndex];
  RenderResourceSet next_storage;
  RenderResourceSet& next = AcquireResourceSetScratch(next_storage);
  next.source_serial = root_source_serial;
  const MTL::RenderStages stages = MetalAllGraphicsRenderStages();
  constexpr size_t kGraphicsRootStage = kStageVertex;
  const StageRootArgumentAllocation& allocation =
      current_bindless_stage_root_arguments_[kGraphicsRootStage];
  if (allocation.valid) {
    AddRenderResourceRef(next, allocation.buffer, MTL::ResourceUsageRead,
                         stages);
  }
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    for (size_t cbv = 0; cbv < kCbvSlotCount; ++cbv) {
      if (uniforms.cbvs[stage][cbv].active) {
        AddRenderResourceRef(next, uniforms.cbvs[stage][cbv].buffer,
                             MTL::ResourceUsageRead, stages);
      }
    }
  }
  PublishRenderResourceSet(current_bindless_root_resource_set_,
                           std::move(next));
  current_bindless_root_resource_source_serial_ =
      current_bindless_root_resource_set_.source_serial;
}

void MetalCommandProcessor::NoteTexturesSampledByCurrentCommandBuffer(
    const RenderResourceSet& set) {
  // [texture-precommit] every cache texture a draw samples reaches the render
  // encoder through this set (once per encoder), as the texture itself or a
  // swizzle view of it; remember the root texture for this command buffer.
  for (const RenderResourceRef& ref : set.resources) {
    if (!ref.resource) {
      continue;
    }
    if (!cb_sampled_textures_.insert(ref.resource).second) {
      continue;
    }
    MTL::Texture* root = static_cast<MTL::Texture*>(ref.resource);
    for (int depth = 0; depth < 4; ++depth) {
      MTL::Texture* parent = root->parentTexture();
      if (!parent) {
        break;
      }
      root = parent;
    }
    if (root != ref.resource) {
      cb_sampled_textures_.insert(root);
    }
  }
}

bool MetalCommandProcessor::IsTextureSampledByCurrentCommandBuffer(
    const MTL::Texture* texture) const {
  return texture && cb_sampled_textures_.count(texture) != 0;
}

int MetalCommandProcessor::TexRenameQueuedDrawsSample(
    const MTL::Texture* root, const void* texture) const {
  int result = 0;
  for (const PreparedDrawPtr& draw : prepared_draw_queue_) {
    if (draw->texture_materialization_plan.PlansLoadOf(texture)) return 1;
    for (const RenderResourceRef& ref : draw->texture_resource_set.resources) {
      if (!ref.resource) continue;
      const MTL::Texture* t = static_cast<const MTL::Texture*>(ref.resource);
      for (int depth = 0; depth < 5 && t; ++depth) {
        if (t == root) {
          result = 2;
          break;
        }
        t = const_cast<MTL::Texture*>(t)->parentTexture();
      }
    }
  }
  return result;
}

MTL::CommandBuffer* MetalCommandProcessor::TexRenameBeginSettle(
    const char* why) {
  if (current_render_encoder_) {
    render_end_detail_ = why;
    EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kTextureBlit);
  if (!EnsureCommandBuffer()) return nullptr;
  return current_command_buffer_;
}

void MetalCommandProcessor::TexRenameAfterLoads(
    const MetalTextureCache::TextureMaterializationPlan* const* plans,
    size_t count) {
  if (!texture_cache_ || !MetalTextureCache::TexRenameEnabled() || !count) {
    return;
  }
  MTL::CommandBuffer* cmd = nullptr;
  if (texture_cache_->TexRenameNeedsSettle(plans, count)) {
    cmd = TexRenameBeginSettle(MetalTextureCache::TexRenameVerify()
                                   ? "texture:rename_verify"
                                   : "texture:rename_settle");
  }
  texture_cache_->TexRenameSettleEncode(plans, count, cmd);
}

void MetalCommandProcessor::TexRenameSettleOrphansNow() {
  if (!texture_cache_ || !texture_cache_->TexRenameHasOrphans()) return;
  MTL::CommandBuffer* cmd = nullptr;
  if (texture_cache_->TexRenameAnyPending()) {
    cmd = TexRenameBeginSettle("texture:rename_orphan");
  }
  texture_cache_->TexRenameSettleOrphans(cmd);
}

void MetalCommandProcessor::ApplyRenderEncoderResourceSets() {
  ApplyRenderEncoderResourceSet(
      RenderResourceSetKind::kFixed, current_bindless_fixed_resource_set_,
      render_encoder_bindless_fixed_resources_serial_);
  ApplyRenderEncoderResourceSet(
      RenderResourceSetKind::kTexture, current_bindless_texture_resource_set_,
      render_encoder_bindless_texture_resources_serial_);
  ApplyRenderEncoderResourceSet(RenderResourceSetKind::kRoot,
                                current_bindless_root_resource_set_,
                                render_encoder_bindless_root_resources_serial_);
}

void MetalCommandProcessor::ApplyRenderEncoderResourceSet(
    RenderResourceSetKind kind, const RenderResourceSet& set,
    uint64_t& applied_serial) {
  const size_t kind_index = static_cast<size_t>(kind);
  if (!current_render_encoder_) {
    return;
  }
  if (applied_serial == set.serial) {
    if (kind_index < backend_telemetry_.render_resource_set_skips.size()) {
      ++backend_telemetry_.render_resource_set_skips[kind_index];
    }
    return;
  }
  if (kind_index < backend_telemetry_.render_resource_set_applies.size()) {
    ++backend_telemetry_.render_resource_set_applies[kind_index];
    backend_telemetry_.render_resource_set_resources[kind_index] +=
        set.resources.size();
    ++backend_telemetry_.render_resource_registry_registers[kind_index];
  }
  if (kind == RenderResourceSetKind::kTexture) {
    NoteTexturesSampledByCurrentCommandBuffer(set);
  }
  constexpr size_t kResourceBatchSize = 128;
  std::array<const MTL::Resource*, kResourceBatchSize> batch;
  uint32_t batch_count = 0;
  MTL::ResourceUsage batch_usage = MTL::ResourceUsageRead;
  MTL::RenderStages batch_stages = MTL::RenderStages(0);
  auto flush_batch = [&]() {
    if (!batch_count) {
      return;
    }
    UseRenderEncoderResources(batch.data(), batch_count, batch_usage,
                              batch_stages);
    batch_count = 0;
  };
  for (const RenderResourceRef& ref : set.resources) {
    if (!ref.resource) {
      continue;
    }
    if (!batch_count) {
      batch_usage = ref.usage;
      batch_stages = ref.stages;
    } else if (MetalResourceUsageBits(batch_usage) !=
                   MetalResourceUsageBits(ref.usage) ||
               MetalRenderStageBits(batch_stages) !=
                   MetalRenderStageBits(ref.stages)) {
      flush_batch();
      batch_usage = ref.usage;
      batch_stages = ref.stages;
    }
    batch[batch_count++] = ref.resource;
    if (batch_count == batch.size()) {
      flush_batch();
    }
  }
  flush_batch();
  applied_serial = set.serial;
}

void MetalCommandProcessor::GrowRenderEncoderResourceUsageTable(
    size_t min_capacity) {
  size_t capacity = render_encoder_resource_usage_table_.size();
  if (!capacity) {
    capacity = 256;
  }
  while (capacity < min_capacity) {
    capacity <<= 1;
  }

  std::vector<EncoderResourceUsageTableEntry> old_entries =
      std::move(render_encoder_resource_usage_table_);
  render_encoder_resource_usage_table_.assign(capacity, {});
  render_encoder_resource_usage_count_ = 0;
  for (const EncoderResourceUsageTableEntry& old_entry : old_entries) {
    if (!old_entry.resource) {
      continue;
    }
    bool inserted = false;
    EncoderResourceUsageState* state =
        FindOrInsertRenderEncoderResourceUsage(old_entry.resource, inserted);
    if (state) {
      *state = old_entry.state;
    }
  }
}

MetalCommandProcessor::EncoderResourceUsageState*
MetalCommandProcessor::FindOrInsertRenderEncoderResourceUsage(
    MTL::Resource* resource, bool& inserted) {
  inserted = false;
  if (!resource) {
    return nullptr;
  }
  if (render_encoder_resource_usage_table_.empty() ||
      (render_encoder_resource_usage_count_ + 1) * 4 >=
          render_encoder_resource_usage_table_.size() * 3) {
    GrowRenderEncoderResourceUsageTable(
        std::max<size_t>(render_encoder_resource_usage_table_.size() * 2, 256));
  }

  const size_t mask = render_encoder_resource_usage_table_.size() - 1;
  size_t index =
      ((reinterpret_cast<uintptr_t>(resource) >> 4) * 11400714819323198485ull) &
      mask;
  for (;;) {
    EncoderResourceUsageTableEntry& entry =
        render_encoder_resource_usage_table_[index];
    if (!entry.resource) {
      entry.resource = resource;
      entry.state = {};
      ++render_encoder_resource_usage_count_;
      inserted = true;
      return &entry.state;
    }
    if (entry.resource == resource) {
      return &entry.state;
    }
    index = (index + 1) & mask;
  }
}

void MetalCommandProcessor::ResetRenderEncoderBufferBindings() {
  for (auto& stage_bindings : render_encoder_buffer_bindings_) {
    for (auto& binding : stage_bindings) {
      binding = {};
    }
  }
  render_encoder_bindless_stage_root_bind_serials_.fill(0);
  render_encoder_bindless_table_bind_mesh_path_ = false;
  render_encoder_bindless_table_bind_tessellation_ = false;
}

void MetalCommandProcessor::InvalidateRenderEncoderBufferBinding(
    RenderEncoderBufferStage stage, NS::UInteger index) {
  const size_t stage_index = size_t(stage);
  if (stage_index >= render_encoder_buffer_bindings_.size() ||
      index >= kTrackedRenderEncoderBufferBindingCount) {
    return;
  }
  render_encoder_buffer_bindings_[stage_index][index] = {};
  if (index == kIRArgumentBufferBindPoint ||
      index == kIRArgumentBufferHullDomainBindPoint) {
    if (stage == RenderEncoderBufferStage::kFragment) {
      render_encoder_bindless_stage_root_bind_serials_[kStagePixel] = 0;
    } else {
      render_encoder_bindless_stage_root_bind_serials_[kStageVertex] = 0;
      render_encoder_bindless_table_bind_mesh_path_ = false;
      render_encoder_bindless_table_bind_tessellation_ = false;
    }
  }
}

void MetalCommandProcessor::SetRenderEncoderBuffer(
    RenderEncoderBufferStage stage, MTL::Buffer* buffer, NS::UInteger offset,
    NS::UInteger index) {
  if (!current_render_encoder_ || stage == RenderEncoderBufferStage::kCount) {
    return;
  }
  size_t stage_index = static_cast<size_t>(stage);
  auto set_buffer = [&](MTL::Buffer* buffer_to_set,
                        NS::UInteger offset_to_set) {
    switch (stage) {
      case RenderEncoderBufferStage::kVertex:
        current_render_encoder_->setVertexBuffer(buffer_to_set, offset_to_set,
                                                 index);
        break;
      case RenderEncoderBufferStage::kFragment:
        current_render_encoder_->setFragmentBuffer(buffer_to_set, offset_to_set,
                                                   index);
        break;
      case RenderEncoderBufferStage::kObject:
        current_render_encoder_->setObjectBuffer(buffer_to_set, offset_to_set,
                                                 index);
        break;
      case RenderEncoderBufferStage::kMesh:
        current_render_encoder_->setMeshBuffer(buffer_to_set, offset_to_set,
                                               index);
        break;
      case RenderEncoderBufferStage::kCount:
        break;
    }
  };
  auto set_buffer_offset = [&]() {
    switch (stage) {
      case RenderEncoderBufferStage::kVertex:
        current_render_encoder_->setVertexBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kFragment:
        current_render_encoder_->setFragmentBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kObject:
        current_render_encoder_->setObjectBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kMesh:
        current_render_encoder_->setMeshBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kCount:
        break;
    }
  };
  if (!buffer) {
    if (stage_index <
        backend_telemetry_.render_encoder_buffer_null_binds.size()) {
      ++backend_telemetry_.render_encoder_buffer_null_binds[stage_index];
    }
    set_buffer(nullptr, 0);
    InvalidateRenderEncoderBufferBinding(stage, index);
    return;
  }
  if (index >= kTrackedRenderEncoderBufferBindingCount) {
    if (stage_index <
        backend_telemetry_.render_encoder_buffer_untracked_binds.size()) {
      ++backend_telemetry_.render_encoder_buffer_untracked_binds[stage_index];
    }
    set_buffer(buffer, offset);
    return;
  }
  auto& binding = render_encoder_buffer_bindings_[stage_index][index];
  if (binding.valid && binding.buffer == buffer) {
    if (binding.offset != offset) {
      if (stage_index <
          backend_telemetry_.render_encoder_buffer_offset_binds.size()) {
        ++backend_telemetry_.render_encoder_buffer_offset_binds[stage_index];
      }
      set_buffer_offset();
      binding.offset = offset;
    } else if (stage_index <
               backend_telemetry_.render_encoder_buffer_noop_binds.size()) {
      ++backend_telemetry_.render_encoder_buffer_noop_binds[stage_index];
    }
    return;
  }
  if (stage_index <
      backend_telemetry_.render_encoder_buffer_full_binds.size()) {
    ++backend_telemetry_.render_encoder_buffer_full_binds[stage_index];
  }
  set_buffer(buffer, offset);
  binding = {buffer, offset, true};
}

void MetalCommandProcessor::SetRenderEncoderVertexBuffer(MTL::Buffer* buffer,
                                                         NS::UInteger offset,
                                                         NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kVertex, buffer, offset,
                         index);
}

void MetalCommandProcessor::SetRenderEncoderFragmentBuffer(MTL::Buffer* buffer,
                                                           NS::UInteger offset,
                                                           NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kFragment, buffer, offset,
                         index);
}

void MetalCommandProcessor::SetRenderEncoderObjectBuffer(MTL::Buffer* buffer,
                                                         NS::UInteger offset,
                                                         NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kObject, buffer, offset,
                         index);
}

void MetalCommandProcessor::SetRenderEncoderMeshBuffer(MTL::Buffer* buffer,
                                                       NS::UInteger offset,
                                                       NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kMesh, buffer, offset,
                         index);
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage) {
  UseRenderEncoderResource(resource, usage, MetalAllGraphicsRenderStages());
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage,
                                                     MTL::RenderStages stages) {
  if (!current_render_encoder_ || !resource) {
    return;
  }
  if (IsResidencySetResourceCovered(resource)) {
    ++backend_telemetry_.residency_set_use_resources_covered;
    return;
  }
  ++backend_telemetry_.residency_set_use_resources_fallback;
  ++backend_telemetry_.render_encoder_use_resource_calls;

  const uint32_t usage_bits = MetalResourceUsageBits(usage);
  uint32_t stage_bits = MetalRenderStageBits(stages);
  if (!stage_bits) {
    stages = MetalAllGraphicsRenderStages();
    stage_bits = MetalRenderStageBits(stages);
  }

  auto usage_state_covers = [&](const EncoderResourceUsageState& state) {
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) &&
        ((state.read_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) &&
        ((state.write_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) &&
        ((state.sample_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    return (state.usage_bits & usage_bits) == usage_bits;
  };
  auto add_usage_to_state = [&](EncoderResourceUsageState& state) {
    state.usage_bits |= usage_bits;
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) {
      state.read_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) {
      state.write_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) {
      state.sample_stage_bits |= stage_bits;
    }
  };

  bool inserted = false;
  EncoderResourceUsageState* state =
      FindOrInsertRenderEncoderResourceUsage(resource, inserted);
  if (!state) {
    return;
  }
  if (!inserted) {
    if (usage_state_covers(*state)) {
      ++backend_telemetry_.render_encoder_use_resource_skips;
      return;
    }
    ++backend_telemetry_.render_encoder_use_resource_upgrades;
  }

  add_usage_to_state(*state);
  current_render_encoder_->useResource(resource, usage, stages);
}

void MetalCommandProcessor::UseRenderEncoderResources(
    const MTL::Resource* const resources[], uint32_t count,
    MTL::ResourceUsage usage) {
  UseRenderEncoderResources(resources, count, usage,
                            MetalAllGraphicsRenderStages());
}

void MetalCommandProcessor::UseRenderEncoderResources(
    const MTL::Resource* const resources[], uint32_t count,
    MTL::ResourceUsage usage, MTL::RenderStages stages) {
  if (!current_render_encoder_ || !resources || !count) {
    return;
  }
  const uint32_t usage_bits = MetalResourceUsageBits(usage);
  uint32_t stage_bits = MetalRenderStageBits(stages);
  if (!stage_bits) {
    stages = MetalAllGraphicsRenderStages();
    stage_bits = MetalRenderStageBits(stages);
  }
  auto usage_state_covers = [&](const EncoderResourceUsageState& state) {
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) &&
        ((state.read_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) &&
        ((state.write_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) &&
        ((state.sample_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    return (state.usage_bits & usage_bits) == usage_bits;
  };
  auto add_usage_to_state = [&](EncoderResourceUsageState& state) {
    state.usage_bits |= usage_bits;
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) {
      state.read_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) {
      state.write_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) {
      state.sample_stage_bits |= stage_bits;
    }
  };

  constexpr size_t kResourceBatchSize = 128;
  std::array<const MTL::Resource*, kResourceBatchSize> resource_batch;
  uint32_t resource_batch_count = 0;
  auto flush_batch = [&]() {
    if (!resource_batch_count) {
      return;
    }
    current_render_encoder_->useResources(resource_batch.data(),
                                          resource_batch_count, usage, stages);
    ++backend_telemetry_.render_encoder_use_resources_batches;
    resource_batch_count = 0;
  };
  for (uint32_t i = 0; i < count; ++i) {
    const MTL::Resource* const_resource = resources[i];
    if (!const_resource) {
      continue;
    }
    MTL::Resource* resource = const_cast<MTL::Resource*>(const_resource);
    if (IsResidencySetResourceCovered(resource)) {
      ++backend_telemetry_.residency_set_use_resources_covered;
      continue;
    }
    ++backend_telemetry_.residency_set_use_resources_fallback;
    ++backend_telemetry_.render_encoder_use_resources_requested;
    bool inserted = false;
    EncoderResourceUsageState* state =
        FindOrInsertRenderEncoderResourceUsage(resource, inserted);
    if (!state) {
      continue;
    }
    if (!inserted) {
      if (usage_state_covers(*state)) {
        ++backend_telemetry_.render_encoder_use_resources_skips;
        continue;
      }
      ++backend_telemetry_.render_encoder_use_resource_upgrades;
    }

    add_usage_to_state(*state);
    resource_batch[resource_batch_count++] = const_resource;
    if (resource_batch_count == resource_batch.size()) {
      flush_batch();
    }
  }
  flush_batch();
}

void MetalCommandProcessor::UseRenderEncoderHeap(MTL::Heap* heap) {
  if (!current_render_encoder_ || !heap) {
    return;
  }
  // See IsResidencySetResourceCovered: heap-backed textures and render targets
  // still need this usage declaration until texture / RT hazards are tracked
  // explicitly by the backend.
  ++backend_telemetry_.residency_set_use_heaps_fallback;
  for (MTL::Heap* used_heap : render_encoder_heap_usage_) {
    if (used_heap == heap) {
      return;
    }
  }
  render_encoder_heap_usage_.push_back(heap);
  current_render_encoder_->useHeap(heap);
}

void MetalCommandProcessor::UseRenderEncoderAttachmentHeaps(
    MTL::RenderPassDescriptor* descriptor) {
  if (!current_render_encoder_ || !descriptor) {
    return;
  }
  auto* color_attachments = descriptor->colorAttachments();
  // [encoder-trim] the render-target cache binds colour attachments 0..3 only
  // (GetRenderPassDescriptor); 4..7 never hold a texture.
  const uint32_t color_attachment_count = EncoderTrimEnabled() ? 4 : 8;
  for (uint32_t i = 0; i < color_attachment_count; ++i) {
    auto* attachment = color_attachments->object(i);
    if (!attachment) {
      continue;
    }
    MTL::Texture* texture = attachment->texture();
    if (texture) {
      UseRenderEncoderHeap(texture->heap());
    }
  }
  auto* depth_attachment = descriptor->depthAttachment();
  if (depth_attachment && depth_attachment->texture()) {
    UseRenderEncoderHeap(depth_attachment->texture()->heap());
  }
  auto* stencil_attachment = descriptor->stencilAttachment();
  if (stencil_attachment && stencil_attachment->texture()) {
    UseRenderEncoderHeap(stencil_attachment->texture()->heap());
  }
}

void MetalCommandProcessor::PassExtentNoteDrawn(uint32_t x0, uint32_t y0,
                                                uint32_t x1, uint32_t y1) {
  if (x1 <= x0 || y1 <= y0) return;
  if (!pass_drawn_any_) {
    pass_drawn_any_ = true;
    pass_drawn_x0_ = x0;
    pass_drawn_y0_ = y0;
    pass_drawn_x1_ = x1;
    pass_drawn_y1_ = y1;
    return;
  }
  pass_drawn_x0_ = std::min(pass_drawn_x0_, x0);
  pass_drawn_y0_ = std::min(pass_drawn_y0_, y0);
  pass_drawn_x1_ = std::max(pass_drawn_x1_, x1);
  pass_drawn_y1_ = std::max(pass_drawn_y1_, y1);
}

void MetalCommandProcessor::PassExtentRecordEnd() {
  // Draws only: passes with transfers never shrink, and their transfer rows
  // say nothing about what the draws of the same kind of pass need.
  if (pass_extent_key_ && pass_draws_y1_) {
    if (pass_extent_history_.size() > 512) {
      pass_extent_history_.clear();
    }
    PassExtentHistory& h = pass_extent_history_[pass_extent_key_];
    h.rows[h.next] = uint16_t(std::min<uint32_t>(pass_draws_y1_, 65535));
    h.next = uint8_t((h.next + 1) & 15);
  }
  pass_extent_key_ = nullptr;
  pass_extent_h_ = 0;
  pass_drawn_any_ = false;
  pass_draws_y1_ = 0;
}

namespace {
const void* TileResolveTextureBase(MTL::Texture* t) {
  for (int i = 0; t && i < 4; ++i) {
    MTL::Texture* parent = t->parentTexture();
    if (!parent) break;
    t = parent;
  }
  return t;
}
}  // namespace

void MetalCommandProcessor::TileResolveKeep(MTL::Texture* const* written,
                                            uint32_t count) {
  if (!current_render_encoder_) return;
  if (tres_kept_gen_ != render_encoder_gen_) {
    pass_tile_written_.clear();
  }
  tres_kept_gen_ = render_encoder_gen_;
  // The tile pipeline replaced the draw pipeline on the encoder.
  current_render_pipeline_state_ = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    if (written[i]) pass_tile_written_.insert(TileResolveTextureBase(written[i]));
  }
}

bool MetalCommandProcessor::TileResolvePassSampled(MTL::Texture* texture) {
  // Bases are resolved here, once per sampled resource and pass, not per draw.
  if (!texture) return false;
  for (; pass_sampled_based_ < pass_sampled_order_.size(); ++pass_sampled_based_) {
    pass_sampled_bases_.insert(TileResolveTextureBase(static_cast<MTL::Texture*>(
        const_cast<void*>(pass_sampled_order_[pass_sampled_based_]))));
  }
  return pass_sampled_bases_.count(TileResolveTextureBase(texture)) != 0;
}

void MetalCommandProcessor::TileResolveNotePassSampled(
    const RenderResourceSet& set) {
  // The set's serial changes only with its contents.
  if (set.serial && set.serial == pass_sampled_source_serial_) {
    return;
  }
  pass_sampled_source_serial_ = set.serial;
  for (const RenderResourceRef& ref : set.resources) {
    if (ref.resource && pass_sampled_textures_.insert(ref.resource).second) {
      pass_sampled_order_.push_back(ref.resource);
    }
  }
}

bool MetalCommandProcessor::TileResolveKeptHazard(
    const PreparedDraw& draw) const {
  if (!pass_tile_written_.empty()) {
    for (const RenderResourceRef& ref : draw.texture_resource_set.resources) {
      if (ref.resource &&
          pass_tile_written_.count(TileResolveTextureBase(
              static_cast<MTL::Texture*>(ref.resource)))) {
        return true;
      }
    }
  }
  // Shared memory the tile stage of this pass wrote (unscaled resolves):
  // other tiles may not have run yet.
  if (draw.shared_memory_hazard_range_count) {
    for (const PendingSharedMemoryWrite& pending :
         pending_shared_memory_writes_) {
      if (pending.active_render_encoder &&
          (NS::UInteger(pending.producer_stages) &
           NS::UInteger(MTL::RenderStageTile)) &&
          PendingSharedMemoryWriteOverlapsRanges(
              pending, draw.shared_memory_hazard_ranges.data(),
              draw.shared_memory_hazard_range_count)) {
        return true;
      }
    }
  }
  return false;
}

namespace {
// FABLE2_TILE_RESOLVE_TRACE=1: why passes keep their full render area.
void PassExtentTrace(const std::string& what) {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_TRACE");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  if (!on) return;
  static std::map<std::string, uint64_t> counts;
  static uint64_t total = 0;
  counts[what]++;
  if (++total % 4096 == 0) {
    for (const auto& e : counts) {
      REXLOG_INFO("[tile-resolve] extent {:8} {}", e.second, e.first);
    }
    counts.clear();
  }
}
}  // namespace

uint32_t MetalCommandProcessor::PassExtentChoose(
    MTL::RenderPassDescriptor* desc, uint32_t& full_width,
    uint32_t& full_height) {
  pass_extent_key_ = nullptr;
  if (!desc || !MetalRenderTargetCache::TileResolveExtentEnabled()) {
    return 0;
  }
  // Only load-and-keep attachments: outside the render area nothing is
  // loaded, cleared, stored or resolved, so those rows simply stay as they
  // are in memory, which is what a pass that does not draw there leaves.
  uint32_t w = UINT32_MAX, h = UINT32_MAX;
  const void* key = nullptr;
  bool shrinkable = !(render_target_cache_ &&
                      render_target_cache_->HasPendingDrawPassTransfers());
  std::string why = shrinkable ? "" : "transfers ";
  auto check = [&](MTL::RenderPassAttachmentDescriptor* a) {
    if (!a || !a->texture()) return;
    MTL::Texture* t = a->texture();
    if (!key) key = t;
    w = std::min(w, uint32_t(t->width()));
    h = std::min(h, uint32_t(t->height()));
    const MTL::StoreAction store = a->storeAction();
    if (a->loadAction() != MTL::LoadActionLoad ||
        (store != MTL::StoreActionStore &&
         store != MTL::StoreActionDontCare)) {
      shrinkable = false;
      why += fmt::format("load{}store{} ", uint32_t(a->loadAction()),
                         uint32_t(store));
    }
  };
  for (uint32_t i = 0; i < 8; ++i) {
    check(desc->colorAttachments()->object(i));
  }
  check(desc->depthAttachment());
  check(desc->stencilAttachment());
  if (!key || h == UINT32_MAX) return 0;
  // History per (target, rows the opening draw needs): the same kind of
  // pass on a target tends to open with the same draw (predicated tiling
  // draws each screen tile into the same target).
  pass_extent_key_ = reinterpret_cast<const void*>(
      reinterpret_cast<uintptr_t>(key) ^
      (uintptr_t(std::min<uint32_t>(pass_open_need_bottom_, 0xFFFF)) << 48));
  key = pass_extent_key_;
  if (!shrinkable) {
    PassExtentTrace(fmt::format("{}x{} {}", w, h, why));
    return 0;
  }
  uint32_t want = pass_open_need_bottom_;
  auto it = pass_extent_history_.find(key);
  if (it != pass_extent_history_.end()) {
    for (uint16_t r : it->second.rows) want = std::max(want, uint32_t(r));
  }
  // Metal tiles are 16-32 rows; a whole number of them.
  want = std::max<uint32_t>(32, (want + 31) & ~uint32_t(31));
  if (want >= h) {
    if (h >= 256) {
      PassExtentTrace(fmt::format("{}x{} full: need {} history", w, h,
                                  pass_open_need_bottom_));
    }
    return 0;
  }
  PassExtentTrace(fmt::format("{}x{} shrunk to {}", w, h, want));
  full_width = w;
  full_height = h;
  return want;
}

MTL::RenderPassDescriptor* MetalCommandProcessor::GetDrawRenderPassDescriptor(
    bool fallback_depth_attachment_required) {
  if (render_target_cache_) {
    if (MTL::RenderPassDescriptor* cache_desc =
            render_target_cache_->GetRenderPassDescriptor(
                1, fallback_depth_attachment_required)) {
      if (zpd_real_ && zpd_pool_) {
        cache_desc->setVisibilityResultBuffer(zpd_pool_->visibility_buffer());
        cache_desc->setVisibilityResultType(
            MTL::VisibilityResultTypeAccumulate);
      } else {
        cache_desc->setVisibilityResultBuffer(nullptr);
      }
      return cache_desc;
    }
  }
  return nullptr;
}

bool MetalCommandProcessor::BeginRenderEncoderForDraw(
    bool fallback_depth_attachment_required) {
  ++backend_telemetry_.begin_encoder_calls;
  if (!EnsureCommandBuffer()) {
    return false;
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kRenderBegin);
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    return false;
  }

  if (!current_render_encoder_ && (render_encoder_resource_usage_count_ ||
                                   !render_encoder_heap_usage_.empty())) {
    ++backend_telemetry_.begin_encoder_resource_usage_resets;
    ResetRenderEncoderResourceUsage();
  }

  // Obtain the render pass descriptor from MetalRenderTargetCache (host
  // render-target path). If an encoder is already active, keep using its
  // descriptor while the attachment textures still match the current binding.
  // The cache may be dirty only because a clear load action was consumed and
  // the next new pass needs a refreshed descriptor.
  if (current_render_encoder_ && render_target_cache_ &&
      render_target_cache_->IsRenderPassDescriptorCompatible(
          current_render_pass_descriptor_, 1,
          fallback_depth_attachment_required)) {
    ++backend_telemetry_.begin_encoder_reused_compatible;
    return true;
  }

  MTL::RenderPassDescriptor* pass_descriptor =
      GetDrawRenderPassDescriptor(fallback_depth_attachment_required);
  if (!pass_descriptor) {
    ++backend_telemetry_.begin_encoder_descriptor_failures;
    REXLOG_ERROR("BeginRenderEncoderForDraw: No render pass descriptor available");
    return false;
  }

  // Keep first-use depth clears tied to the guest depth format. D24FS8 uses
  // 0.0 as the far value, unlike fixed-point D24S8.
  if (render_target_cache_) {
    if (auto* da = pass_descriptor->depthAttachment()) {
      da->setClearDepth(render_target_cache_->GetDepthTargetClearDepth());
    }
  }

  // If the render pass configuration has changed since the current render
  // encoder was created (e.g. dummy RT0 -> real RTs, depth/stencil binding),
  // restart the render encoder with the updated descriptor.
  if (current_render_encoder_ &&
      current_render_pass_descriptor_ != pass_descriptor) {
    ++backend_telemetry_.begin_encoder_descriptor_restarts;
    EndRenderEncoder(
        RenderEncoderEndReason::kBeginRenderEncoderDescriptorChanged);
  }

  if (!current_render_encoder_) {
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kRenderBegin);
    // If some path cleared the encoder without going through EndRenderEncoder,
    // avoid leaking cached binding state into the new encoder.
    // Note: renderCommandEncoder() returns an autoreleased object, we must
    // retain it.
    uint32_t extent_h = 0;
    {
      ScopedAutoreleasePool autorelease_pool;
      // [ssmask] FABLE2_CLEAR_LOAD_ACTION: whole-attachment deferred clears
      // of this new pass become its clear load actions (restored on the
      // cache's descriptor right after the encoder is made). Before the
      // extent choice: a cleared attachment keeps the full render area.
      const uint32_t clear_la_changed =
          render_target_cache_
              ? render_target_cache_->ApplyClearLoadActions(pass_descriptor)
              : 0;
      // [tile-resolve extent] a render area as tall as this texture's
      // recent passes drew (and this draw needs); 0 keeps the full area.
      uint32_t extent_full_w = 0, extent_full_h = 0;
      extent_h = PassExtentChoose(pass_descriptor, extent_full_w,
                                  extent_full_h);
      if (extent_h) {
        pass_descriptor->setRenderTargetWidth(extent_full_w);
        pass_descriptor->setRenderTargetHeight(extent_h);
      }
      current_render_pass_trace_ =
          pass_trace::BeginRender(current_command_buffer_, pass_descriptor, "draw");
      current_render_encoder_ =
          current_command_buffer_->renderCommandEncoder(pass_descriptor);
      if (extent_h) {
        // The descriptor is the cache's; everything else sizes from the
        // attachments as before.
        pass_descriptor->setRenderTargetWidth(0);
        pass_descriptor->setRenderTargetHeight(0);
      }
      MetalRenderTargetCache::RestoreClearLoadActions(pass_descriptor,
                                                      clear_la_changed);
      if (current_render_pass_trace_) {
        pass_trace::ClearRender(pass_descriptor);
      }
      if (current_render_encoder_) {
        current_render_encoder_->retain();
        if (EncoderDebugLabelsEnabled()) {
          current_render_encoder_->setLabel(NS::String::string(
              "XeniaRenderEncoder", NS::UTF8StringEncoding));
        }
      }
    }
    if (!current_render_encoder_) {
      ++backend_telemetry_.begin_encoder_creation_failures;
      REXLOG_ERROR("Failed to create render command encoder");
      return false;
    }
    ++backend_telemetry_.begin_encoder_created;
    pass_extent_h_ = extent_h;
    pass_drawn_any_ = false;
    pass_draws_y1_ = 0;
    pass_sampled_textures_.clear();
    pass_sampled_order_.clear();
    pass_sampled_bases_.clear();
    pass_sampled_based_ = 0;
    pass_tile_written_.clear();
    pass_sampled_source_serial_ = 0;
    if (extent_h) {
      ++UploadTrace().tres_shrunk;
    }
    ResetRenderDependencyDedup();
    // [occlusion-real] the pool's buffer is attached to every pass descriptor.
    current_render_encoder_has_zpd_visibility_ =
        zpd_real_ && zpd_pool_ &&
        pass_descriptor->visibilityResultBuffer() ==
            zpd_pool_->visibility_buffer();
    ResetRenderEncoderBufferBindings();
    current_render_pipeline_state_ = nullptr;
    ff_blend_factor_valid_ = false;
    rasterizer_state_valid_ = false;
    viewport_dirty_ = true;
    scissor_dirty_ = true;
    current_depth_stencil_state_ = nullptr;
    stencil_reference_valid_ = false;
    heap_binds_set_on_encoder_ = false;
    if (current_render_pass_descriptor_ != pass_descriptor) {
      if (current_render_pass_descriptor_) {
        current_render_pass_descriptor_->release();
      }
      current_render_pass_descriptor_ = pass_descriptor;
      current_render_pass_descriptor_->retain();
    }
    UseRenderEncoderAttachmentHeaps(pass_descriptor);

    // Derive the initial viewport/scissor from the active render pass texture.
    // Metal validates scissor rectangles against the descriptor attachments,
    // not against the cache's logical RT0 binding.
    uint32_t rt_width = 1;
    uint32_t rt_height = 1;
    GetActiveRenderTargetSize(pass_descriptor, render_target_cache_.get(), 1280,
                              720, rt_width, rt_height);
    if (pass_extent_h_) {
      rt_height = std::min(rt_height, pass_extent_h_);
    }

    MTL::Viewport viewport = {
        0.0, 0.0, static_cast<double>(rt_width), static_cast<double>(rt_height),
        0.0, 1.0};
    current_render_encoder_->setViewport(viewport);

    MTL::ScissorRect scissor = {0, 0, rt_width, rt_height};
    current_render_encoder_->setScissorRect(scissor);

    // IssueDraw applies the guest viewport/scissor before dispatch.
    viewport_dirty_ = true;
    scissor_dirty_ = true;
  }
  return true;
}

void MetalCommandProcessor::EndCommandBuffer() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kCommandBufferEnd)) {
    REXLOG_ERROR("Metal EndCommandBuffer: failed to flush prepared draw queue");
  }
  TexRenameSettleOrphansNow();
  EndRenderEncoder(RenderEncoderEndReason::kCommandBufferEnd);
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kCommandBufferEnd);
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    REXLOG_ERROR(
        "Metal: failed to flush texture upload encoder before command buffer "
        "end");
  }
  if (pipeline_cache_) {
    pipeline_cache_->EndSubmission();
  }

  if (current_command_buffer_) {
    EncodeMainCommandBufferEnd();
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    submission_has_draws_ = false;
  }
  DrainCommandBufferAutoreleasePool();
}

void MetalCommandProcessor::ApplyDepthStencilState(
    const DrawDynamicState& dynamic_state) {
  if (!current_render_encoder_ || !device_) {
    return;
  }

  bool primitive_polygonal =
      dynamic_state.rasterization_enabled && dynamic_state.primitive_polygonal;
  auto stencil_ref_mask_front = dynamic_state.stencil_ref_mask_front;
  auto stencil_ref_mask_back = dynamic_state.stencil_ref_mask_back;
  auto depth_control = dynamic_state.depth_control;

  bool has_stencil_attachment = false;
  if (current_render_pass_descriptor_) {
    if (auto* stencil_attachment =
            current_render_pass_descriptor_->stencilAttachment()) {
      has_stencil_attachment = stencil_attachment->texture() != nullptr;
    }
  }

  if (!has_stencil_attachment && depth_control.stencil_enable) {
    static bool no_stencil_logged = false;
    if (!no_stencil_logged) {
      no_stencil_logged = true;
      REXLOG_WARN(
          "Metal: stencil enabled but no stencil attachment bound; disabling "
          "stencil for this pass");
    }
    depth_control.stencil_enable = 0;
    depth_control.backface_enable = 0;
    depth_control.stencilfunc = xenos::CompareFunction::kAlways;
    depth_control.stencilfail = xenos::StencilOp::kKeep;
    depth_control.stencilzpass = xenos::StencilOp::kKeep;
    depth_control.stencilzfail = xenos::StencilOp::kKeep;
    depth_control.stencilfunc_bf = xenos::CompareFunction::kAlways;
    depth_control.stencilfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzpass_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzfail_bf = xenos::StencilOp::kKeep;
    stencil_ref_mask_front.value = 0;
    stencil_ref_mask_back.value = 0;
  }

  DepthStencilStateKey key;
  key.depth_control = depth_control.value;
  key.stencil_ref_mask_front = stencil_ref_mask_front.value;
  key.stencil_ref_mask_back = stencil_ref_mask_back.value;
  key.polygonal_and_backface = (primitive_polygonal ? 1u : 0u) |
                               (depth_control.backface_enable ? 2u : 0u);

  MTL::DepthStencilState* state = nullptr;
  auto it = depth_stencil_state_cache_.find(key);
  if (it != depth_stencil_state_cache_.end()) {
    state = it->second;
  } else {
    MTL::DepthStencilDescriptor* ds_desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    if (depth_control.z_enable) {
      ds_desc->setDepthCompareFunction(
          ToMetalCompareFunction(depth_control.zfunc));
      ds_desc->setDepthWriteEnabled(depth_control.z_write_enable != 0);
    } else {
      ds_desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
      ds_desc->setDepthWriteEnabled(false);
    }

    if (depth_control.stencil_enable) {
      auto* front = MTL::StencilDescriptor::alloc()->init();
      front->setStencilCompareFunction(
          ToMetalCompareFunction(depth_control.stencilfunc));
      front->setStencilFailureOperation(
          ToMetalStencilOperation(depth_control.stencilfail));
      front->setDepthFailureOperation(
          ToMetalStencilOperation(depth_control.stencilzfail));
      front->setDepthStencilPassOperation(
          ToMetalStencilOperation(depth_control.stencilzpass));
      front->setReadMask(stencil_ref_mask_front.stencilmask);
      front->setWriteMask(stencil_ref_mask_front.stencilwritemask);

      ds_desc->setFrontFaceStencil(front);

      if (primitive_polygonal && depth_control.backface_enable) {
        auto* back = MTL::StencilDescriptor::alloc()->init();
        back->setStencilCompareFunction(
            ToMetalCompareFunction(depth_control.stencilfunc_bf));
        back->setStencilFailureOperation(
            ToMetalStencilOperation(depth_control.stencilfail_bf));
        back->setDepthFailureOperation(
            ToMetalStencilOperation(depth_control.stencilzfail_bf));
        back->setDepthStencilPassOperation(
            ToMetalStencilOperation(depth_control.stencilzpass_bf));
        back->setReadMask(stencil_ref_mask_back.stencilmask);
        back->setWriteMask(stencil_ref_mask_back.stencilwritemask);
        ds_desc->setBackFaceStencil(back);
        back->release();
      } else {
        ds_desc->setBackFaceStencil(front);
      }

      front->release();
    }

    state = device_->newDepthStencilState(ds_desc);
    ds_desc->release();

    if (!state) {
      REXLOG_ERROR("Failed to create Metal depth/stencil state");
      return;
    }
    depth_stencil_state_cache_.emplace(key, state);
  }

  if (current_depth_stencil_state_ != state) {
    current_render_encoder_->setDepthStencilState(state);
    current_depth_stencil_state_ = state;
  }

  if (depth_control.stencil_enable) {
    uint32_t ref_front = stencil_ref_mask_front.stencilref;
    uint32_t ref_back = stencil_ref_mask_back.stencilref;
    uint32_t ref = ref_front;
    if (primitive_polygonal && depth_control.backface_enable &&
        dynamic_state.pa_su_sc_mode_cntl.cull_front &&
        !dynamic_state.pa_su_sc_mode_cntl.cull_back) {
      ref = ref_back;
    } else if (primitive_polygonal && depth_control.backface_enable &&
               ref_front != ref_back) {
      static bool mismatch_logged = false;
      if (!mismatch_logged) {
        mismatch_logged = true;
        REXLOG_WARN(
            "Metal: front/back stencil ref differ (front={}, back={}); using "
            "front for both",
            ref_front, ref_back);
      }
    }
    if (!stencil_reference_valid_ || current_stencil_reference_ != ref) {
      current_render_encoder_->setStencilReferenceValue(ref);
      current_stencil_reference_ = ref;
      stencil_reference_valid_ = true;
    }
  }
}

void MetalCommandProcessor::ApplyRasterizerState(
    const DrawDynamicState& dynamic_state) {
  if (!current_render_encoder_ || !render_target_cache_) {
    return;
  }

  if (!rasterizer_state_valid_ ||
      current_cull_mode_ != dynamic_state.cull_mode) {
    current_render_encoder_->setCullMode(dynamic_state.cull_mode);
    current_cull_mode_ = dynamic_state.cull_mode;
  }

  if (!rasterizer_state_valid_ ||
      current_front_facing_winding_ != dynamic_state.front_facing_winding) {
    current_render_encoder_->setFrontFacingWinding(
        dynamic_state.front_facing_winding);
    current_front_facing_winding_ = dynamic_state.front_facing_winding;
  }

  if (!rasterizer_state_valid_ ||
      current_triangle_fill_mode_ != dynamic_state.triangle_fill_mode) {
    current_render_encoder_->setTriangleFillMode(
        dynamic_state.triangle_fill_mode);
    current_triangle_fill_mode_ = dynamic_state.triangle_fill_mode;
  }

  float depth_bias_values[] = {dynamic_state.depth_bias_constant,
                               dynamic_state.depth_bias_slope, 0.0f};
  if (!rasterizer_state_valid_ ||
      std::memcmp(current_depth_bias_values_, depth_bias_values,
                  sizeof(depth_bias_values)) != 0) {
    current_render_encoder_->setDepthBias(dynamic_state.depth_bias_constant,
                                          dynamic_state.depth_bias_slope, 0.0f);
    std::memcpy(current_depth_bias_values_, depth_bias_values,
                sizeof(depth_bias_values));
  }

  if (!rasterizer_state_valid_ ||
      current_depth_clip_mode_ != dynamic_state.depth_clip_mode) {
    current_render_encoder_->setDepthClipMode(dynamic_state.depth_clip_mode);
    current_depth_clip_mode_ = dynamic_state.depth_clip_mode;
  }
  rasterizer_state_valid_ = true;
}

void MetalCommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal,
    uint32_t line_loop_closing_index, xenos::Endian index_endian,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  uint32_t dirty = 0u;
  ArchFloatMask dirty_float_mask = floatmask_zero;

  auto update_dirty_floatmask = [&dirty_float_mask](float x, float y) {
    dirty_float_mask =
        ArchORFloatMask(dirty_float_mask, ArchCmpneqFloatMask(x, y));
  };
  auto update_dirty_uint32_cmp = [&dirty](uint32_t x, uint32_t y) {
    dirty |= (x ^ y);
  };

  // Get color info for each render target
  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  // Build flags
  uint32_t flags = 0;

  // Shared memory mode - determines whether shaders read from SRV (T0) or UAV
  // (U0)
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }

  // W0 division control from PA_CL_VTE_CNTL
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }

  // Primitive type flags
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }

  // Depth format
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }

  // Alpha test - encode compare function in flags
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;

  // Gamma conversion flags for render targets
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }

  update_dirty_uint32_cmp(system_constants_.flags, flags);
  system_constants_.flags = flags;

  // Tessellation factor range
  float tessellation_factor_min, tessellation_factor_max;
  GetTessellationFactorRange(regs, tessellation_factor_min,
                             tessellation_factor_max);
  update_dirty_floatmask(system_constants_.tessellation_factor_range_min,
                         tessellation_factor_min);
  update_dirty_floatmask(system_constants_.tessellation_factor_range_max,
                         tessellation_factor_max);
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index
  update_dirty_uint32_cmp(system_constants_.line_loop_closing_index,
                          line_loop_closing_index);
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Vertex index configuration
  update_dirty_uint32_cmp(
      static_cast<uint32_t>(system_constants_.vertex_index_endian),
      static_cast<uint32_t>(index_endian));
  update_dirty_uint32_cmp(system_constants_.vertex_index_offset,
                          vgt_indx_offset);
  update_dirty_uint32_cmp(system_constants_.vertex_index_min, vgt_min_vtx_indx);
  update_dirty_uint32_cmp(system_constants_.vertex_index_max, vgt_max_vtx_indx);
  system_constants_.vertex_index_endian = index_endian;
  system_constants_.vertex_index_offset = vgt_indx_offset;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (when not CLIP_DISABLE)
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining,
                                &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const float* user_clip_plane_regs = reinterpret_cast<const float*>(
          &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4]);
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs,
                      4 * sizeof(float)) != 0) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs,
                    4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // NDC scale and offset from viewport info
  for (uint32_t i = 0; i < 3; ++i) {
    update_dirty_floatmask(system_constants_.ndc_scale[i],
                           viewport_info.ndc_scale[i]);
    update_dirty_floatmask(system_constants_.ndc_offset[i],
                           viewport_info.ndc_offset[i]);
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size parameters
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    update_dirty_floatmask(system_constants_.point_vertex_diameter_min,
                           point_vertex_diameter_min);
    update_dirty_floatmask(system_constants_.point_vertex_diameter_max,
                           point_vertex_diameter_max);
    update_dirty_floatmask(system_constants_.point_constant_diameter[0],
                           point_constant_diameter_x);
    update_dirty_floatmask(system_constants_.point_constant_diameter[1],
                           point_constant_diameter_y);
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // Screen to NDC radius conversion.
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader. Include draw_resolution_scale to
    // match D3D12 behavior.
    const bool point_native_scale =
        render_target_cache_ &&
        render_target_cache_->IsCurrentPassNativeScale();
    uint32_t point_draw_resolution_scale_x =
        (render_target_cache_ && !point_native_scale)
            ? render_target_cache_->draw_resolution_scale_x()
            : 1;
    uint32_t point_draw_resolution_scale_y =
        (render_target_cache_ && !point_native_scale)
            ? render_target_cache_->draw_resolution_scale_y()
            : 1;
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(point_draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(point_draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    update_dirty_floatmask(
        system_constants_.point_screen_diameter_to_ndc_radius[0],
        point_screen_diameter_to_ndc_radius_x);
    update_dirty_floatmask(
        system_constants_.point_screen_diameter_to_ndc_radius[1],
        point_screen_diameter_to_ndc_radius_y);
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / resolution scaling (mirror D3D12 logic).
  // Always update textures_resolution_scaled, even when used_texture_mask is 0,
  // to avoid stale values from previous draws.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    if (texture_cache_) {
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = (texture_index & 3) * 8;
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = uint32_t(0xFF) << texture_signs_shift;
      update_dirty_uint32_cmp((texture_signs_uint & texture_signs_mask),
                              texture_signs_shifted);
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
  }
  update_dirty_uint32_cmp(system_constants_.textures_resolution_scaled,
                          textures_resolution_scaled);
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;
  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT check: a draw given the variant
  // without the footprint code must have no scaled texture behind an offset
  // fetch of a non-depth format (tr_n_fp_variant_miss stays 0).
  if (!fp_variant_chosen_ &&
      (textures_resolution_scaled & fp_variant_check_mask_)) {
    ++UploadTrace().fp_variant_miss;
  }
  fp_variant_check_mask_ = 0;
  fp_variant_chosen_ = false;

  // Sample count log2 for alpha to mask
  uint32_t sample_count_log2_x =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  update_dirty_uint32_cmp(system_constants_.sample_count_log2[0],
                          sample_count_log2_x);
  update_dirty_uint32_cmp(system_constants_.sample_count_log2[1],
                          sample_count_log2_y);
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test reference
  update_dirty_floatmask(system_constants_.alpha_test_reference, rb_alpha_ref);
  system_constants_.alpha_test_reference = rb_alpha_ref;

  // Alpha to mask
  uint32_t alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                               ? (rb_colorcontrol.value >> 24) | (1 << 8)
                               : 0;
  update_dirty_uint32_cmp(system_constants_.alpha_to_mask, alpha_to_mask);
  system_constants_.alpha_to_mask = alpha_to_mask;

  // Color exponent bias
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    // Fixed-point render targets (k_16_16 / k_16_16_16_16) are backed by
    // *_SNORM in the host render targets path. If full-range emulation is
    // requested, remap from -32...32 to -1...1 by dividing the output values
    // by 32.
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_16_16) {
      if (!render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    } else if (color_infos[i].color_format ==
               xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (!render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale = rex::memory::Reinterpret<float>(
        int32_t(0x3F800000 + (color_exp_bias << 23)));
    update_dirty_floatmask(system_constants_.color_exp_bias[i],
                           color_exp_bias_scale);
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  // Blend constants (used by EDRAM and for host blending)
  float blend_red = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  float blend_green = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  float blend_blue = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  float blend_alpha = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  update_dirty_floatmask(system_constants_.edram_blend_constant[0], blend_red);
  update_dirty_floatmask(system_constants_.edram_blend_constant[1],
                         blend_green);
  update_dirty_floatmask(system_constants_.edram_blend_constant[2], blend_blue);
  update_dirty_floatmask(system_constants_.edram_blend_constant[3],
                         blend_alpha);
  system_constants_.edram_blend_constant[0] = blend_red;
  system_constants_.edram_blend_constant[1] = blend_green;
  system_constants_.edram_blend_constant[2] = blend_blue;
  system_constants_.edram_blend_constant[3] = blend_alpha;

  dirty |= ArchFloatMaskSignbit(dirty_float_mask);
  cbuffer_binding_system_.up_to_date &= !dirty;
}


}  // namespace metal
}  // namespace gpu
}  // namespace xe

bool rex::graphics::metal::MetalCommandProcessor::DrainForCheckpoint() {
  EndCommandBuffer();
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while (completed_command_buffers_.load(std::memory_order_acquire)<submission_current_ ||
         pending_completion_handlers_.load(std::memory_order_acquire)!=0) {
    if (std::chrono::steady_clock::now()>=until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

bool rex::graphics::metal::MetalCommandProcessor::PauseForCheckpoint() {
  if (paused_ || checkpoint_barrier_) return false;
  auto barrier=std::make_shared<CheckpointBarrier>();
  checkpoint_barrier_=barrier;
  CallInThread([this,barrier] {
    bool succeeded=DrainForCheckpoint();
    std::unique_lock guard(barrier->mutex);
    barrier->succeeded=succeeded;
    barrier->ready=true;
    barrier->changed.notify_all();
    if (succeeded) barrier->changed.wait(guard,[&] { return barrier->released; });
  });
  std::unique_lock guard(barrier->mutex);
  if (!barrier->changed.wait_for(guard,std::chrono::seconds(7),[&] { return barrier->ready; }) ||
      !barrier->succeeded) {
    barrier->released=true;
    barrier->changed.notify_all();
    checkpoint_barrier_.reset();
    return false;
  }
  paused_=true;
  return true;
}

void rex::graphics::metal::MetalCommandProcessor::ResumeFromCheckpoint() {
  auto barrier=std::move(checkpoint_barrier_);
  if (!barrier) return;
  std::lock_guard guard(barrier->mutex);
  paused_=false;
  barrier->released=true;
  barrier->changed.notify_all();
}

bool rex::graphics::metal::MetalCommandProcessor::ParkRingForReinit(uint32_t timeout_us,
                                                                    uint32_t* old_write,
                                                                    uint32_t* read_at_start,
                                                                    uint32_t* waited_us) {
  constexpr uint32_t kParked = 0xBAADF00D;  // WorkerThreadMain: "no commands"
  const auto start = std::chrono::steady_clock::now();
  const auto until = start + std::chrono::microseconds(timeout_us);
  // read_ptr_index_ is a plain member written by the worker; read it atomically here.
  auto read_index = [this] { return __atomic_load_n(&read_ptr_index_, __ATOMIC_ACQUIRE); };
  *read_at_start = read_index();
  uint32_t spins = 0;
  bool parked = false;
  for (;;) {
    uint32_t w = write_ptr_index_.load(std::memory_order_acquire);
    *old_write = w;
    if (w == kParked) {  // already parked (nothing to drain)
      parked = true;
      break;
    }
    // The worker only executes while read != write and sets read = write when the batch
    // returns, so read == w means it is idle (or about to idle) with nothing pending. Park it
    // then: with write == sentinel it cannot start a batch when the init resets read to 0.
    if (read_index() == w && write_ptr_index_.compare_exchange_strong(w, kParked)) {
      parked = true;
      break;
    }
    if (std::chrono::steady_clock::now() >= until) break;
    if (++spins < 64) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  *waited_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
  return parked;
}

void rex::graphics::metal::MetalCommandProcessor::FinishRingReinit() {
  // Same effect as the guest writing CP_RB_WPTR = 0 (GraphicsSystem::WriteRegister).
  register_file_->values[0x01C5] = 0;  // CP_RB_WPTR
  UpdateWritePointer(0);
}
