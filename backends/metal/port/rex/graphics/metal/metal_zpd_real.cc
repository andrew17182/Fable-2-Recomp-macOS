// [occlusion-real] Real Xenos occlusion queries (EVENT_WRITE_ZPD) on Metal
// visibility results. Default on; FABLE2_OCCLUSION_REAL=0 = fake answer. CR-04,
// docs/occlusion-queries-20261014.md.
//
// Guest contract (Fable II's D3D, read from the recompiled code):
//   - Issue(BEGIN) points RB_SAMPLE_COUNT_ADDR at the BEGIN record (END + 0x20)
//     and sends EVENT_WRITE_ZPD.
//   - Issue(END) stores 0xFFFFFEED in END.ZPass_A/B, points the register at the
//     END record and sends EVENT_WRITE_ZPD. Under predicated tiling every tile
//     replay has its own record pair (bin-select predicated SET_CONSTANT).
//   - GetData (sub_8220BE78) sums END.ZPass(A+B) - BEGIN.ZPass(A+B) over the
//     query's records. While the last record still holds the sentinel it
//     returns S_FALSE with *pData = 1 ("visible").
//   - The object-culling path (sub_8220BDD8) spins on GetData until S_OK; the
//     flare path (sub_821D8310) keeps last frame's value on S_FALSE.
//
// So the sentinel stays in the END record until every draw of the query has
// finished on the GPU; the command-buffer completion handler then writes the
// real count. When the guest spins, the CP ring goes idle and PrepareForWait
// commits the command buffer, so the wait lasts as long as the GPU work.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <string>

#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/xenos.h"
#include "rex/logging.h"
#include "rex/memory.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {

constexpr uint32_t kZpdSlotCapacity = 32768;  // 256 KB of 8-byte results
constexpr uint32_t kRecordAlignMask = ~uint32_t(0x1F);
constexpr uint32_t kSentinelBE = 0xFFFFFEEDu;
constexpr uint32_t kSentinelLE = 0xEDFEFFFFu;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool StatsEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_ZPD_STATS");
    return v && v[0] == '1';
  }();
  return enabled;
}

bool HasSentinel(const xenos::xe_gpu_depth_sample_counts* c) {
  auto is = [](uint32_t v) { return v == kSentinelBE || v == kSentinelLE; };
  return is(c->ZPass_A) || is(c->ZPass_B) || is(c->ZFail_A) || is(c->ZFail_B);
}

// Draw-skip reasons that produce no samples on Xenos either, so a query whose
// only missing draws are these stays exact.
bool SkipReasonProducesNoSamples(const char* reason) {
  return reason && (std::strcmp(reason, "ds_rast_off") == 0 ||
                    std::strcmp(reason, "ds_no_host_vertices") == 0);
}

// Result-ready signal for the guest-side wait (src/diagnostics/
// fable2_zpd_wait.h): bumped after every guest result write, so the object-
// culling path can sleep between GetData polls instead of spinning.
std::mutex g_result_mutex;
std::condition_variable g_result_cv;
std::atomic<uint64_t> g_result_generation{0};
std::atomic<bool> g_real_enabled{false};

void SignalResults() {
  {
    std::lock_guard<std::mutex> lock(g_result_mutex);
    g_result_generation.fetch_add(1, std::memory_order_release);
  }
  g_result_cv.notify_all();
}

}  // namespace

// Exported for the app (found with dlsym; the Vulkan build has neither).
extern "C" __attribute__((visibility("default"), used)) bool
fable2_zpd_real_enabled() {
  return g_real_enabled.load(std::memory_order_acquire);
}
extern "C" __attribute__((visibility("default"), used)) uint64_t
fable2_zpd_result_generation() {
  return g_result_generation.load(std::memory_order_acquire);
}
// Waits until a result is written after `seen` was read, or the guard expires.
// Returns false only when the guard fired.
extern "C" __attribute__((visibility("default"), used)) bool
fable2_zpd_wait_result(uint64_t seen, uint32_t guard_us) {
  std::unique_lock<std::mutex> lock(g_result_mutex);
  return g_result_cv.wait_for(lock, std::chrono::microseconds(guard_us), [&] {
    return g_result_generation.load(std::memory_order_acquire) != seen;
  });
}

void MetalCommandProcessor::ZpdInitialize() {
  // Default on (2026-10-14); FABLE2_OCCLUSION_REAL=0 restores the SDK's fake
  // "1000 samples passed" answer.
  const char* v = std::getenv("FABLE2_OCCLUSION_REAL");
  zpd_real_ = !(v && v[0] == '0');
  if (!zpd_real_) {
    return;
  }
  zpd_pool_ = std::make_unique<MetalZPDVisibilityPool>();
  if (!zpd_pool_->EnsureInitialized(device_, kZpdSlotCapacity)) {
    REXLOG_WARN("[occlusion-real] visibility buffer allocation failed; "
                "using the fake sample count");
    zpd_pool_.reset();
    zpd_real_ = false;
    return;
  }
  AddResidencySetResource(zpd_pool_->visibility_buffer());
  g_real_enabled.store(true, std::memory_order_release);
  REXLOG_INFO("[occlusion-real] on: {} visibility slots", kZpdSlotCapacity);
}

void MetalCommandProcessor::ZpdShutdown() {
  g_real_enabled.store(false, std::memory_order_release);
  SignalResults();
  std::lock_guard<std::mutex> lock(zpd_mutex_);
  zpd_pending_segments_.clear();
  zpd_reports_.clear();
  zpd_active_report_ = 0;
  zpd_span_open_ = false;
  if (zpd_pool_) {
    zpd_pool_->Shutdown();
    zpd_pool_.reset();
  }
}

void MetalCommandProcessor::ZpdWriteGuestResult(uint32_t end_record,
                                                uint32_t samples) {
  auto* c = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
      end_record);
  if (!c) {
    return;
  }
  // Same layout as the SDK's fake path (Total_A = ZPass_A = n). The guest
  // tests the ZPass_A/B pair for the sentinel, so that pair goes last, as one
  // 64-bit store.
  c->Total_A = samples;
  c->Total_B = 0;
  c->ZFail_A = 0;
  c->ZFail_B = 0;
  c->StencilFail_A = 0;
  c->StencilFail_B = 0;
  std::atomic_thread_fence(std::memory_order_release);
  uint64_t pair = 0;
  xenos::xe_gpu_depth_sample_counts tmp{};
  tmp.ZPass_A = samples;
  tmp.ZPass_B = 0;
  std::memcpy(&pair, &tmp.ZPass_A, sizeof(pair));
  __atomic_store_n(reinterpret_cast<uint64_t*>(&c->ZPass_A), pair,
                   __ATOMIC_RELEASE);
  SignalResults();
}

void MetalCommandProcessor::ZpdFinalizeLocked(uint32_t report_id,
                                              ZpdReport& report) {
  (void)report_id;
  auto seq_it = zpd_record_seq_.find(report.end_record);
  if (seq_it == zpd_record_seq_.end() || seq_it->second != report.seq) {
    // The guest re-began this record; a later query owns it.
    ++zpd_stats_.stale;
    return;
  }
  uint64_t n = report.samples;
  if (report.uncertain) {
    ++zpd_stats_.uncertain;
    n = std::max<uint64_t>(n, 1);  // a draw may be missing: never cull on it
  }
  uint32_t samples = uint32_t(std::min<uint64_t>(n, 0xFFFFFFFFu));
  ZpdWriteGuestResult(report.end_record, samples);
  ++zpd_stats_.results;
  if (!samples) {
    ++zpd_stats_.zero_results;
  }
  if (report.end_ns) {
    uint64_t latency = NowNs() - report.end_ns;
    zpd_stats_.latency_ns += latency;
    uint64_t prev = zpd_stats_.latency_max_ns.load(std::memory_order_relaxed);
    while (latency > prev &&
           !zpd_stats_.latency_max_ns.compare_exchange_weak(prev, latency)) {
    }
  }
}

bool MetalCommandProcessor::ZpdExecuteEvent(memory::RingBuffer* reader,
                                            uint32_t packet, uint32_t count) {
  (void)packet;
  (void)count;
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  const uint32_t address =
      register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR] & kRecordAlignMask;
  auto* counts =
      address ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
                    address)
              : nullptr;
  if (!counts) {
    return true;
  }
  const bool is_end = HasSentinel(counts);

  // Draws queued before the event belong before it (the queue also refuses
  // draws while a query is open).
  FlushPreparedDrawQueue(PreparedDrawFlushReason::kQuery);

  std::lock_guard<std::mutex> lock(zpd_mutex_);
  if (!is_end) {
    ++zpd_stats_.begins;
    if (zpd_active_report_) {
      // One sample-count address register: a new BEGIN implicitly ends the
      // open report. Fable does not do this; finish it as "visible".
      ++zpd_stats_.overlaps;
      auto it = zpd_reports_.find(zpd_active_report_);
      if (it != zpd_reports_.end()) {
        it->second.ended = true;
        it->second.uncertain = true;
        it->second.end_record = it->second.begin_record - 0x20;
        it->second.end_ns = NowNs();
        if (!it->second.pending) {
          ZpdFinalizeLocked(it->first, it->second);
          zpd_reports_.erase(it);
        }
      }
      zpd_active_report_ = 0;
    }
    // Xenos writes the running counter here; D3D only reads END - BEGIN, so
    // BEGIN = 0 and END = the count (as the SDK's fake path does).
    std::memset(counts, 0, sizeof(*counts));
    uint32_t id = zpd_next_report_++;
    if (!id) {
      id = zpd_next_report_++;
    }
    ZpdReport& report = zpd_reports_[id];
    report.begin_record = address;
    // D3D's END record sits 0x20 below BEGIN; a new BEGIN invalidates any
    // in-flight result for that record.
    report.seq = ++zpd_record_seq_[address - 0x20];
    report.end_record = address - 0x20;
    zpd_active_report_ = id;
    return true;
  }

  ++zpd_stats_.ends;
  auto it = zpd_active_report_ ? zpd_reports_.find(zpd_active_report_)
                               : zpd_reports_.end();
  zpd_active_report_ = 0;
  if (it == zpd_reports_.end()) {
    // END without a BEGIN we saw: answer "visible" now so the guest never
    // waits on a sentinel nobody will clear.
    ++zpd_stats_.orphan_ends;
    ZpdWriteGuestResult(address, 1);
    return true;
  }
  ZpdReport& report = it->second;
  if (report.end_record != address) {
    report.end_record = address;
    report.seq = ++zpd_record_seq_[address];
  }
  report.ended = true;
  report.end_ns = NowNs();
  if (report.dispatched < report.guest_draws) {
    report.uncertain = true;
  }
  if (!report.pending) {
    ZpdFinalizeLocked(it->first, report);
    zpd_reports_.erase(it);
  }
  return true;
}

void MetalCommandProcessor::ZpdBeginDrawSpan() {
  if (!zpd_real_ || !zpd_active_report_ || !current_render_encoder_) {
    return;
  }
  std::lock_guard<std::mutex> lock(zpd_mutex_);
  auto it = zpd_reports_.find(zpd_active_report_);
  if (it == zpd_reports_.end()) {
    return;
  }
  ZpdReport& report = it->second;
  if (!current_render_encoder_has_zpd_visibility_) {
    ++zpd_stats_.no_visibility;
    report.uncertain = true;
    return;
  }
  uint32_t slot = 0, generation = 0;
  size_t offset = 0;
  if (!zpd_pool_ || !zpd_pool_->Acquire(slot, generation, offset)) {
    ++zpd_stats_.slot_exhausted;
    report.uncertain = true;
    return;
  }
  uint32_t scale = 1;
  if (render_target_cache_ && !render_target_cache_->IsCurrentPassNativeScale()) {
    scale = render_target_cache_->draw_resolution_scale_x() *
            render_target_cache_->draw_resolution_scale_y();
  }
  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeCounting, NS::UInteger(offset));
  zpd_pending_segments_[submission_current_].push_back(
      ZpdSegment{zpd_active_report_, slot, generation, std::max(scale, 1u)});
  ++report.pending;
  ++report.dispatched;
  ++zpd_stats_.segments;
  zpd_span_open_ = true;
  zpd_span_encoder_gen_ = render_encoder_gen_;
}

void MetalCommandProcessor::ZpdEndDrawSpan(bool dispatched) {
  if (!zpd_span_open_) {
    return;
  }
  zpd_span_open_ = false;
  const bool same_encoder =
      current_render_encoder_ && zpd_span_encoder_gen_ == render_encoder_gen_;
  if (same_encoder) {
    current_render_encoder_->setVisibilityResultMode(
        MTL::VisibilityResultModeDisabled, 0);
    if (dispatched) {
      return;
    }
  } else {
    // The draw moved to another encoder part-way: its samples may be missing.
    ++zpd_stats_.encoder_changed;
  }
  std::lock_guard<std::mutex> lock(zpd_mutex_);
  auto it = zpd_reports_.find(zpd_active_report_);
  if (it != zpd_reports_.end()) {
    it->second.uncertain = true;
  }
}

void MetalCommandProcessor::ZpdNoteIssueDraw(bool dispatched_before_valid,
                                             uint32_t dispatched_before) {
  if (!dispatched_before_valid) {
    return;
  }
  std::lock_guard<std::mutex> lock(zpd_mutex_);
  auto it = zpd_reports_.find(zpd_active_report_);
  if (it == zpd_reports_.end()) {
    return;
  }
  ZpdReport& report = it->second;
  ++report.guest_draws;
  if (report.dispatched != dispatched_before) {
    return;
  }
  // Not dispatched (or deferred): exact only if Xenos would not draw it either.
  const char* reason = draw_skip_reason_;
  if (SkipReasonProducesNoSamples(reason)) {
    ++report.dispatched;  // accounted for
  } else {
    ++zpd_stats_.skipped_draws;
  }
  if (StatsEnabled()) {
    ++zpd_skip_reasons_[reason ? reason : "none"];
  }
}

void MetalCommandProcessor::ZpdOnSubmissionCompleted(uint64_t submission,
                                                     bool error) {
  std::lock_guard<std::mutex> lock(zpd_mutex_);
  auto seg_it = zpd_pending_segments_.find(submission);
  if (seg_it == zpd_pending_segments_.end()) {
    return;
  }
  std::vector<ZpdSegment> segments = std::move(seg_it->second);
  zpd_pending_segments_.erase(seg_it);
  for (const ZpdSegment& seg : segments) {
    uint64_t raw = 0;
    if (zpd_pool_) {
      raw = zpd_pool_->Read(seg.slot);
      zpd_pool_->Release(seg.slot, seg.generation);
    }
    auto it = zpd_reports_.find(seg.report);
    if (it == zpd_reports_.end()) {
      continue;
    }
    ZpdReport& report = it->second;
    // Guest-scale samples, rounded up so any covered host sample stays >= 1.
    report.samples += (raw + seg.scale - 1) / seg.scale;
    if (error) {
      report.uncertain = true;
    }
    if (report.pending) {
      --report.pending;
    }
    if (report.ended && !report.pending) {
      ZpdFinalizeLocked(it->first, report);
      zpd_reports_.erase(it);
    }
  }
}

void MetalCommandProcessor::ZpdMaybeLogStats() {
  if (!StatsEnabled() || !zpd_real_) {
    return;
  }
  if (frame_current_ < zpd_stats_last_frame_ + 600) {
    return;
  }
  zpd_stats_last_frame_ = frame_current_;
  const uint64_t results = zpd_stats_.results.load();
  std::string reasons;
  for (const auto& r : zpd_skip_reasons_) {
    reasons += r.first + "=" + std::to_string(r.second) + " ";
  }
  size_t in_flight = 0;
  {
    std::lock_guard<std::mutex> lock(zpd_mutex_);
    in_flight = zpd_reports_.size();
  }
  REXLOG_INFO(
      "[occlusion-real] frame {}: begins {} ends {} results {} zero {} "
      "uncertain {} stale {} orphan {} overlap {} segments {} skipped_draws {} "
      "encoder_changed {} no_vis {} slot_exhausted {} in_flight {} "
      "end->write avg {:.3f} ms max {:.3f} ms | skip reasons: {}",
      frame_current_, zpd_stats_.begins.load(), zpd_stats_.ends.load(), results,
      zpd_stats_.zero_results.load(), zpd_stats_.uncertain.load(),
      zpd_stats_.stale.load(), zpd_stats_.orphan_ends.load(),
      zpd_stats_.overlaps.load(), zpd_stats_.segments.load(),
      zpd_stats_.skipped_draws.load(), zpd_stats_.encoder_changed.load(),
      zpd_stats_.no_visibility.load(), zpd_stats_.slot_exhausted.load(),
      in_flight,
      results ? double(zpd_stats_.latency_ns.load()) / results / 1e6 : 0.0,
      double(zpd_stats_.latency_max_ns.load()) / 1e6, reasons);
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
