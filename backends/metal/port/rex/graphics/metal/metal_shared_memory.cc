/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_shared_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "rex/cvar.h"
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/memory/utils.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/hash.h"

DEFINE_bool(
    metal_shared_memory_direct_write, true,
    "Directly copy safe Metal shared-memory uploads into the shared storage "
    "buffer instead of staging them through a blit.",
    "Metal");

namespace rex {
namespace graphics {
namespace metal {

namespace {
constexpr uint64_t kPreservedExportLifetimeUs = 150000;  // ~9 frames at 60 FPS
constexpr size_t kPreservedExportMaxEntries = 256;
constexpr uint64_t kPreservedExportMaxBytes = 32ull << 20;
constexpr uint32_t kPreservedExportMaxRangeBytes = 1u << 20;
uint64_t PreserveNowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
bool PreserveTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_DRAW_SKIP_TRACE");
    return v && v[0] == '1';
  }();
  return enabled;
}
}  // namespace

UploadTraceCounters& UploadTrace() {
  static UploadTraceCounters counters;  // GPU Commands thread only
  return counters;
}

bool MetalSharedMemory::UploadPrecommitEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_UPLOAD_PRECOMMIT");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[upload-precommit] {} (FABLE2_UPLOAD_PRECOMMIT={})",
        on ? "on: staged uploads of pages last read only by committed command "
             "buffers go to a command buffer committed ahead of the current "
             "one instead of ending the render pass"
           : "off: every staged upload is encoded in the current command buffer",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

MetalSharedMemory::PageAccess MetalSharedMemory::ClassifyPageAccessLocked(
    uint32_t page, uint64_t completed, uint64_t committed,
    uint64_t* last_access) const {
  uint64_t access = 0;
  if (page < page_last_main_gpu_access_submission_.size()) {
    access = page_last_main_gpu_access_submission_[page];
  }
  if (last_access) {
    *last_access = access;
  }
  if (page < page_standalone_gpu_access_counts_.size() &&
      page_standalone_gpu_access_counts_[page]) {
    return PageAccess::kStandalone;
  }
  if (access <= completed) {
    return PageAccess::kCompleted;
  }
  if (access <= committed) {
    return PageAccess::kInflight;
  }
  return PageAccess::kCurrent;
}

bool MetalSharedMemory::MemexportPreserveEnabled() {
  static const bool enabled = [] {
    // Default on (2026-10-09); FABLE2_MEMEXPORT_PRESERVE=0 disables.
    const char* v = std::getenv("FABLE2_MEMEXPORT_PRESERVE");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[memexport-preserve] {} (FABLE2_MEMEXPORT_PRESERVE={})",
        on ? "on: re-uploads keep GPU-written memexport data unless the "
             "guest bytes changed"
           : "off",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

void MetalSharedMemory::ExpirePreservedExports(uint64_t now_us) {
  preserved_exports_.erase(
      std::remove_if(preserved_exports_.begin(), preserved_exports_.end(),
                     [now_us](const PreservedExport& e) {
                       return now_us - e.last_export_us >
                              kPreservedExportLifetimeUs;
                     }),
      preserved_exports_.end());
}

void MetalSharedMemory::NoteMemexportWrite(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(start) + length, kBufferSize));
  static uint32_t logged = 0;
  if (logged < 40) {
    ++logged;
    REXLOG_INFO("[memexport-preserve] export range 0x{:08X} len={} entries={}",
                start, end - start, preserved_exports_.size());
  }
  if (end - start > kPreservedExportMaxRangeBytes) {
    return;  // keep hashing bounded; huge exports are not preserved
  }
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  if (!guest) {
    return;
  }
  const uint64_t now_us = PreserveNowUs();
  ExpirePreservedExports(now_us);
  const uint64_t hash = XXH3_64bits(guest + start, end - start);
  for (PreservedExport& e : preserved_exports_) {
    if (e.start == start && e.end == end) {
      e.guest_hash = hash;
      e.last_export_us = now_us;
      return;
    }
  }
  // Overlapping but different ranges: drop the older ones (keep it simple).
  preserved_exports_.erase(
      std::remove_if(preserved_exports_.begin(), preserved_exports_.end(),
                     [start, end](const PreservedExport& e) {
                       return e.start < end && start < e.end;
                     }),
      preserved_exports_.end());
  uint64_t total = end - start;
  for (const PreservedExport& e : preserved_exports_) total += e.end - e.start;
  if (preserved_exports_.size() >= kPreservedExportMaxEntries ||
      total > kPreservedExportMaxBytes) {
    return;
  }
  preserved_exports_.push_back({start, end, hash, now_us});
}

bool MetalSharedMemory::ShmemValidFirstEnabled() {
  // CR-02 (docs/cr-fixes-20261007.md). On unless FABLE2_SHMEM_VALID_FIRST=0;
  // the Dev Launcher always passes an explicit value.
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_SHMEM_VALID_FIRST");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO("[shmem-valid-first] {} (FABLE2_SHMEM_VALID_FIRST={})",
                on ? "on: direct writes mark pages valid before copying"
                   : "off: direct writes copy, then mark valid",
                v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalSharedMemory::MemexportReadbackEnabled() {
  static const bool enabled = [] {
    // Default on (2026-10-09); FABLE2_MEMEXPORT_READBACK=0 disables.
    const char* v = std::getenv("FABLE2_MEMEXPORT_READBACK");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[memexport-readback] {} (FABLE2_MEMEXPORT_READBACK={})",
        on ? "on: memexport ranges are copied back from the GPU buffer to "
             "guest RAM after their command buffer completes"
           : "off",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

namespace {
constexpr uint32_t kReadbackMaxRangeBytes = 8u << 20;
constexpr size_t kReadbackMaxPending = 4096;
}  // namespace

void MetalSharedMemory::NoteMemexportReadback(uint32_t start, uint32_t length,
                                              uint64_t submission) {
  if (!length || start >= kBufferSize || !submission) {
    return;
  }
  uint32_t end =
      uint32_t(std::min<uint64_t>(uint64_t(start) + length, kBufferSize));
  if (end - start > kReadbackMaxRangeBytes) {
    static uint32_t logged_big = 0;
    if (logged_big < 8) {
      ++logged_big;
      REXLOG_WARN("[memexport-readback] range 0x{:08X} len={} too large, "
                  "not read back",
                  start, end - start);
    }
    return;
  }
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  if (!guest) {
    return;
  }
  if (pending_readbacks_.size() >= kReadbackMaxPending) {
    // Should not happen (completions are processed every command buffer);
    // drop the oldest so bookkeeping stays bounded.
    pending_readbacks_.pop_front();
  }
  pending_readbacks_.push_back({start, end, submission,
                                XXH3_64bits(guest + start, end - start),
                                frame_log::CurrentFrame()});
}

void MetalSharedMemory::ApplyCompletedMemexportReadbacks(
    uint64_t completed_submission) {
  if (pending_readbacks_.empty() ||
      pending_readbacks_.front().submission > completed_submission || !buffer_) {
    return;
  }
  // Pairs with the release in the command buffer completion handler; shared
  // storage on Apple Silicon is coherent once the command buffer completed.
  std::atomic_thread_fence(std::memory_order_acquire);
  uint8_t* guest = static_cast<uint8_t*>(memory().TranslatePhysical(0));
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (!guest || !gpu) {
    return;
  }
  const uint64_t t0 = PreserveNowUs();
  // Entries are queued in submission order, so the completed ones are a prefix.
  size_t done = 0;
  while (done < pending_readbacks_.size() &&
         pending_readbacks_[done].submission <= completed_submission) {
    ++done;
  }
  uint64_t bytes = 0;
  uint64_t ranges = 0;
  uint64_t cpu_skip_bytes = 0;
  std::vector<size_t> rebase_pending;
  std::vector<size_t> rebase_preserved;
  for (size_t i = 0; i < done; ++i) {
    const PendingReadback& e = pending_readbacks_[i];
    const uint32_t len = e.end - e.start;
    // A later completed export of the same range copies newer data anyway.
    bool superseded = false;
    for (size_t j = i + 1; j < done; ++j) {
      if (pending_readbacks_[j].start == e.start &&
          pending_readbacks_[j].end == e.end) {
        superseded = true;
        break;
      }
    }
    if (superseded) {
      continue;
    }
    const uint64_t guest_hash_now = XXH3_64bits(guest + e.start, len);
    if (guest_hash_now != e.guest_hash) {
      // The CPU wrote into the export range after the export: its data wins
      // (that write also invalidated the page, so it gets re-uploaded).
      cpu_skip_bytes += len;
      continue;
    }
    // Hashes taken by later exports / fix A that still describe the guest
    // bytes must follow this (non-CPU) write, or it would look like a CPU
    // write to them.
    rebase_pending.clear();
    rebase_preserved.clear();
    for (size_t j = i + 1; j < pending_readbacks_.size(); ++j) {
      const PendingReadback& l = pending_readbacks_[j];
      if (l.end <= e.start || l.start >= e.end) continue;
      const uint64_t h = (l.start == e.start && l.end == e.end)
                             ? guest_hash_now
                             : XXH3_64bits(guest + l.start, l.end - l.start);
      if (h == l.guest_hash) rebase_pending.push_back(j);
    }
    for (size_t j = 0; j < preserved_exports_.size(); ++j) {
      const PreservedExport& p = preserved_exports_[j];
      if (p.end <= e.start || p.start >= e.end) continue;
      const uint64_t h = (p.start == e.start && p.end == e.end)
                             ? guest_hash_now
                             : XXH3_64bits(guest + p.start, p.end - p.start);
      if (h == p.guest_hash) rebase_preserved.push_back(j);
    }
    std::memcpy(guest + e.start, gpu + e.start, len);
    const uint64_t new_hash = XXH3_64bits(guest + e.start, len);
    for (size_t j : rebase_pending) {
      PendingReadback& l = pending_readbacks_[j];
      l.guest_hash = (l.start == e.start && l.end == e.end)
                         ? new_hash
                         : XXH3_64bits(guest + l.start, l.end - l.start);
    }
    for (size_t j : rebase_preserved) {
      PreservedExport& p = preserved_exports_[j];
      p.guest_hash = (p.start == e.start && p.end == e.end)
                         ? new_hash
                         : XXH3_64bits(guest + p.start, p.end - p.start);
    }
    bytes += len;
    ++ranges;
    static uint32_t logged = 0;
    if (logged < 24) {
      ++logged;
      REXLOG_INFO(
          "[memexport-readback] applied 0x{:08X} len={} export_sub={} "
          "completed={} export_frame={} frame={} changed={}",
          e.start, len, e.submission, completed_submission, e.export_frame,
          frame_log::CurrentFrame(), new_hash != guest_hash_now ? 1 : 0);
    }
  }
  pending_readbacks_.erase(pending_readbacks_.begin(),
                           pending_readbacks_.begin() + done);
  if (PreserveTraceEnabled()) {
    const uint64_t frame = frame_log::CurrentFrame();
    if (bytes) {
      frame_log::Record("tr_memexport_readback_bytes", frame, bytes);
      frame_log::Record("tr_memexport_readback_ranges", frame, ranges);
      frame_log::Record("tr_memexport_readback_us", frame,
                        PreserveNowUs() - t0);
    }
    if (cpu_skip_bytes) {
      frame_log::Record("tr_memexport_readback_cpu_skip_bytes", frame,
                        cpu_skip_bytes);
    }
  }
}

bool MetalSharedMemory::UploadVerifyEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_UPLOAD_VERIFY");
    const bool on = v && v[0] == '1';
    if (on) {
      REXLOG_INFO("[upload-verify] on (trace-only): staged copies are checked "
                  "against the GPU buffer after their submission completes");
    }
    return on;
  }();
  return enabled;
}

void MetalSharedMemory::NoteUploadVerify(uint32_t start, uint32_t size,
                                         bool precommit) {
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  if (!guest || !size || pending_verifies_.size() >= 65536) {
    return;
  }
  pending_verifies_.push_back({start, size, XXH3_64bits(guest + start, size),
                               command_processor_.GetCurrentSubmission(),
                               precommit});
}

void MetalSharedMemory::VerifyCompletedUploads(uint64_t completed_submission) {
  if (pending_verifies_.empty() || !buffer_) {
    return;
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (!guest || !gpu) {
    return;
  }
  // [route][0 ok, 1 mismatch, 2 guest changed since the copy]
  static uint64_t counts[2][3] = {};
  static uint64_t checked = 0;
  while (!pending_verifies_.empty() &&
         pending_verifies_.front().submission <= completed_submission) {
    const PendingVerify& v = pending_verifies_.front();
    const int route = v.precommit ? 1 : 0;
    if (XXH3_64bits(guest + v.start, v.size) != v.hash) {
      ++counts[route][2];
    } else if (XXH3_64bits(gpu + v.start, v.size) == v.hash) {
      ++counts[route][0];
    } else {
      ++counts[route][1];
    }
    pending_verifies_.pop_front();
    ++checked;
    if ((checked & (checked - 1)) == 0 || checked % 20000 == 0) {
      REXLOG_INFO(
          "[upload-verify] in-pass ok {} mismatch {} guest-changed {} | "
          "pre-commit ok {} mismatch {} guest-changed {}",
          counts[0][0], counts[0][1], counts[0][2], counts[1][0], counts[1][1],
          counts[1][2]);
    }
  }
}

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     rex::memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::metal::MetalProvider& provider =
      command_processor_.GetMetalProvider();
  MTL::Device* device = provider.GetDevice();

  if (!device) {
    REXLOG_ERROR("Metal device is null in MetalSharedMemory::Initialize");
    return false;
  }

  // Create Metal buffer - similar to D3D12's approach
  // On Apple Silicon, ResourceStorageModeShared gives CPU/GPU access
  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    REXLOG_ERROR("Metal shared memory: Xbox RAM is null");
    return false;
  }

  buffer_ = device->newBuffer(kBufferSize, MTL::ResourceStorageModeShared);
  if (!buffer_) {
    REXLOG_ERROR("Failed to create Metal shared memory buffer");
    return false;
  }

  upload_buffer_pool_ = std::make_unique<MetalUploadBufferPool>(
      device, rex::align(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                        size_t(1) << page_size_log2()));
  page_last_main_gpu_access_submission_.assign(kBufferSize >> page_size_log2(),
                                               0);
  page_standalone_gpu_access_counts_.assign(kBufferSize >> page_size_log2(), 0);
  page_last_gpu_write_submission_.assign(kBufferSize >> page_size_log2(), 0);
  page_last_staged_copy_submission_.assign(kBufferSize >> page_size_log2(), 0);
  page_last_precommit_copy_submission_.assign(kBufferSize >> page_size_log2(),
                                              0);
  (void)MemexportPreserveEnabled();  // logs once at startup when enabled
  (void)MemexportReadbackEnabled();  // logs once at startup when enabled

  return true;
}

void MetalSharedMemory::ClearCache() {
  SharedMemory::ClearCache();
  pending_readbacks_.clear();

  if (upload_buffer_pool_) {
    upload_buffer_pool_->ClearCache();
  }
}

void MetalSharedMemory::MarkGpuAccess(uint32_t start, uint32_t length,
                                      uint64_t submission_index) {
  if (!length || !submission_index ||
      page_last_main_gpu_access_submission_.empty()) {
    return;
  }
  if (start >= kBufferSize) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  if (end <= start) {
    return;
  }
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  page_last = std::min<uint32_t>(
      page_last,
      static_cast<uint32_t>(page_last_main_gpu_access_submission_.size() - 1));
  for (uint32_t page = page_first; page <= page_last; ++page) {
    page_last_main_gpu_access_submission_[page] =
        std::max(page_last_main_gpu_access_submission_[page], submission_index);
  }
}

void MetalSharedMemory::TrackStandaloneGpuAccess(
    MTL::CommandBuffer* command_buffer,
    const std::pair<uint32_t, uint32_t>* ranges, uint32_t range_count) {
  if (!command_buffer || !ranges || !range_count ||
      page_standalone_gpu_access_counts_.empty()) {
    return;
  }
  auto tracked_page_ranges =
      std::make_shared<std::vector<std::pair<uint32_t, uint32_t>>>();
  tracked_page_ranges->reserve(range_count);
  for (uint32_t i = 0; i < range_count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length || start >= kBufferSize) {
      continue;
    }
    uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
    if (end <= start) {
      continue;
    }
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    page_last = std::min<uint32_t>(
        page_last,
        static_cast<uint32_t>(page_standalone_gpu_access_counts_.size() - 1));
    tracked_page_ranges->push_back({page_first, page_last});
  }
  if (tracked_page_ranges->empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    for (const auto& page_range : *tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        ++page_standalone_gpu_access_counts_[page];
      }
    }
  }
  MetalSharedMemory* shared_memory = this;
  command_buffer->addCompletedHandler(^(MTL::CommandBuffer*) {
    std::lock_guard<std::mutex> lock(
        shared_memory->standalone_gpu_access_mutex_);
    for (const auto& page_range : *tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        if (page < shared_memory->page_standalone_gpu_access_counts_.size() &&
            shared_memory->page_standalone_gpu_access_counts_[page]) {
          --shared_memory->page_standalone_gpu_access_counts_[page];
        }
      }
    }
  });
}

void MetalSharedMemory::MarkGpuWrite(uint32_t start, uint32_t length,
                                     uint64_t submission_index,
                                     bool staged_copy) {
  if (!length || !submission_index || start >= kBufferSize ||
      page_last_gpu_write_submission_.empty()) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  std::vector<uint64_t>& pages = staged_copy
                                    ? page_last_staged_copy_submission_
                                    : page_last_gpu_write_submission_;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    pages[page] = std::max(pages[page], submission_index);
  }
}

void MetalSharedMemory::MarkPrecommitCopy(uint32_t start, uint32_t length,
                                          uint64_t submission_index) {
  if (!length || start >= kBufferSize ||
      page_last_precommit_copy_submission_.empty()) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  for (uint32_t page = start >> page_size_log2();
       page <= static_cast<uint32_t>((end - 1) >> page_size_log2()); ++page) {
    page_last_precommit_copy_submission_[page] = submission_index;
  }
}

uint32_t MetalSharedMemory::RangeOpenWriteReasons(uint32_t start,
                                                  uint32_t length) {
  if (!length || start >= kBufferSize ||
      page_last_gpu_write_submission_.empty()) {
    return 0;
  }
  uint32_t reasons = 0;
  const uint64_t committed = command_processor_.GetLastCommittedSubmission();
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (page_last_gpu_write_submission_[page] > committed) reasons |= 1;
    if (page_last_staged_copy_submission_[page] > committed) reasons |= 2;
    if (page_standalone_gpu_access_counts_[page]) reasons |= 4;
    if (page_last_precommit_copy_submission_[page] > committed) reasons |= 32;
  }
  return reasons;
}

const uint8_t* MetalSharedMemory::StableContents(uint32_t start,
                                                uint32_t length) {
  if (!length || !buffer_ || uint64_t(start) + length > kBufferSize ||
      page_last_gpu_write_submission_.empty() || !IsRangeValid(start, length)) {
    return nullptr;
  }
  const uint8_t* contents = static_cast<const uint8_t*>(buffer_->contents());
  if (!contents) return nullptr;
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = (start + length - 1) >> page_size_log2();
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (page_last_gpu_write_submission_[page] > completed ||
        page_last_staged_copy_submission_[page] > completed ||
        page_last_precommit_copy_submission_[page] > completed ||
        page_standalone_gpu_access_counts_[page]) {
      return nullptr;
    }
  }
  return contents + start;
}

bool MetalSharedMemory::RangeTouchedByOpenOrStandalone(uint32_t start,
                                                       uint32_t length) {
  if (!length || start >= kBufferSize ||
      page_last_main_gpu_access_submission_.empty()) {
    return false;
  }
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  const uint64_t committed = command_processor_.GetLastCommittedSubmission();
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    const PageAccess access =
        ClassifyPageAccessLocked(page, completed, committed, nullptr);
    if (access == PageAccess::kCurrent || access == PageAccess::kStandalone) {
      return true;
    }
  }
  return false;
}

MetalSharedMemory::UploadRouteInfo MetalSharedMemory::GetUploadRouteInfo(
    const SharedMemory::Range* ranges, uint32_t range_count) {
  UploadRouteInfo info;
  if (!ranges || !range_count) {
    return info;
  }

  const uint32_t page_size = 1u << page_size_log2();
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();
  const uint64_t committed_submission =
      command_processor_.GetLastCommittedSubmission();
  const bool direct_write_enabled = REXCVAR_GET(metal_shared_memory_direct_write) &&
                                    buffer_ && buffer_->contents() != nullptr;
  const bool precommit_allowed =
      command_processor_.SharedMemoryPrecommitAllowed();

  std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
  for (uint32_t i = 0; i < range_count; ++i) {
    const SharedMemory::Range& range = ranges[i];
    if (!range.length || range.start >= kBufferSize) {
      continue;
    }
    uint64_t end =
        std::min(uint64_t(range.start) + range.length, uint64_t(kBufferSize));
    if (end <= range.start) {
      continue;
    }
    uint32_t page_first = range.start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint32_t page_length = static_cast<uint32_t>(
          std::min<uint64_t>(page_size, uint64_t(kBufferSize) - page_start));
      if (!page_length ||
          IsRangeValid(static_cast<uint32_t>(page_start), page_length)) {
        continue;
      }

      info.upload_bytes += page_length;
      const PageAccess access = ClassifyPageAccessLocked(
          page, completed_submission, committed_submission, nullptr);
      switch (access) {
        case PageAccess::kCompleted:
          ++info.pages_completed;
          break;
        case PageAccess::kInflight:
          ++info.pages_inflight;
          break;
        case PageAccess::kCurrent:
          ++info.pages_current;
          break;
        case PageAccess::kStandalone:
          ++info.pages_standalone;
          break;
      }
      if (direct_write_enabled && access == PageAccess::kCompleted) {
        info.direct_bytes += page_length;
      } else {
        info.staged_bytes += page_length;
        if (precommit_allowed && access == PageAccess::kInflight) {
          info.precommit_bytes += page_length;
        } else {
          info.inpass_staged_bytes += page_length;
        }
      }
    }
  }
  return info;
}

bool MetalSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  static bool first_upload = true;
  if (first_upload) {
    first_upload = false;
    const uint32_t page_size = 1u << page_size_log2();
    REXLOG_DEBUG("MetalSharedMemory::UploadRanges: page_size={}, {} ranges to upload",
           page_size, num_upload_ranges);
    for (uint32_t i = 0; i < std::min(5u, num_upload_ranges); i++) {
      uint32_t start_byte = upload_page_ranges[i].first * page_size;
      uint32_t length_bytes = upload_page_ranges[i].second * page_size;
      REXLOG_DEBUG("  Range[{}]: page={} count={} -> byte offset=0x{:08X} length={}",
             i, upload_page_ranges[i].first, upload_page_ranges[i].second,
             start_byte, length_bytes);
    }
  }

  if (!buffer_ || num_upload_ranges == 0) {
    return true;
  }

  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    REXLOG_ERROR("MetalSharedMemory::UploadRanges: Xbox RAM is null");
    return false;
  }
  uint8_t* xbox_data = static_cast<uint8_t*>(xbox_ram);

  const uint32_t page_size = 1u << page_size_log2();
  if (!upload_buffer_pool_) {
    REXLOG_ERROR("MetalSharedMemory::UploadRanges: upload buffer pool is null");
    return false;
  }
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  if (MemexportReadbackEnabled()) {
    // Bring guest RAM up to date with completed memexport output first, so
    // the re-upload below writes the GPU's data back rather than stale bytes.
    ApplyCompletedMemexportReadbacks(
        command_processor_.GetCompletedSubmission());
  }

  MTL::BlitCommandEncoder* blit_encoder = nullptr;
  auto get_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
    if (!blit_encoder) {
      blit_encoder = command_processor_.GetSharedMemoryUploadBlitEncoder();
    }
    return blit_encoder;
  };

  uint32_t merged_start = 0;
  uint32_t merged_end = 0;
  bool have_merged = false;

  auto copy_guest_bytes = [&](uint8_t* dest, uint32_t offset, size_t size) {
    if (size < (1ULL << 32) && size > 8192) {
      memory::vastcpy(dest, xbox_data + offset, static_cast<uint32_t>(size));
      swcache::WriteFence();
    } else {
      std::memcpy(dest, xbox_data + offset, size);
    }
  };

  uint64_t direct_eligible_bytes = 0;
  uint64_t staged_required_bytes = 0;
  uint8_t* shared_buffer_contents = static_cast<uint8_t*>(buffer_->contents());
  const bool has_shared_buffer_contents = shared_buffer_contents != nullptr;
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();
  const uint64_t committed_submission =
      command_processor_.GetLastCommittedSubmission();
  // [upload-precommit] trace: categories of the staged pages seen so far,
  // reported as the cause when the first staged copy ends a render pass.
  bool staged_has_current = false;
  bool staged_has_standalone = false;
  bool staged_has_inflight = false;
  // [upload-precommit] decided once per call; see
  // MetalCommandProcessor::SharedMemoryPrecommitAllowed.
  const bool precommit_allowed =
      has_shared_buffer_contents &&
      command_processor_.SharedMemoryPrecommitAllowed();
  uint32_t precommit_pages = 0;
  bool precommit_fell_back = false;

  auto record_direct_write_reject =
      [&](MetalCommandProcessor::SharedMemoryDirectWriteRejectReason reason,
          uint64_t bytes) {
        command_processor_.RecordSharedMemoryDirectWriteReject(reason, bytes);
      };

  // [upload-precommit] route of a run: direct CPU write, staged blit in the
  // current command buffer, or staged blit in a pre-commit command buffer
  // that first waits for main command buffer `wait` (its pages' last reader).
  enum : uint8_t { kRouteStaged = 0, kRouteDirect = 1, kRoutePrecommit = 2 };
  struct UploadRun {
    uint32_t start;
    uint32_t end;
    bool direct_safe;
    uint8_t route;
    uint64_t wait;
  };

  auto append_upload_run = [](std::vector<UploadRun>& runs, uint32_t start,
                              uint32_t end, bool direct_safe,
                              uint8_t route = 0xFF, uint64_t wait = 0) {
    if (end <= start) {
      return;
    }
    if (route == 0xFF) {
      route = direct_safe ? kRouteDirect : kRouteStaged;
    }
    if (!runs.empty() && runs.back().route == route &&
        runs.back().end == start) {
      runs.back().end = end;
      runs.back().wait = std::max(runs.back().wait, wait);
      return;
    }
    runs.push_back({start, end, direct_safe, route, wait});
  };

  auto build_upload_runs = [&](uint32_t start, uint32_t end,
                               std::vector<UploadRun>& runs) {
    if (end <= start) {
      return;
    }
    if (!has_shared_buffer_contents) {
      uint64_t bytes = uint64_t(end) - start;
      staged_required_bytes += bytes;
      record_direct_write_reject(
          MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
              kNoSharedBufferContents,
          bytes);
      append_upload_run(runs, start, end, false);
      return;
    }
    bool saw_direct = false;
    bool saw_staged = false;
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = (end - 1) >> page_size_log2();
    std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint64_t page_end = page_start + page_size;
      uint64_t byte_start = std::max<uint64_t>(page_start, start);
      uint64_t byte_end = std::min<uint64_t>(page_end, end);
      uint64_t bytes = byte_end - byte_start;
      if (!bytes) {
        continue;
      }

      uint64_t last_access = 0;
      const PageAccess access = ClassifyPageAccessLocked(
          page, completed_submission, committed_submission, &last_access);
      UploadTraceCounters& trace = UploadTrace();
      if (access == PageAccess::kStandalone) {
        ++trace.pg_standalone;
        staged_has_standalone = true;
        staged_required_bytes += bytes;
        saw_staged = true;
        append_upload_run(runs, static_cast<uint32_t>(byte_start),
                          static_cast<uint32_t>(byte_end), false);
        record_direct_write_reject(
            MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                kStandaloneAccessInFlight,
            bytes);
        continue;
      }

      if (access != PageAccess::kCompleted) {
        if (access == PageAccess::kCurrent) {
          ++trace.pg_current;
          staged_has_current = true;
        } else {
          ++trace.pg_inflight;
          if (precommit_allowed) {
            // Read only by committed command buffers: copy in a command
            // buffer committed ahead of the current one, after last_access
            // has finished on the GPU, instead of ending the render pass.
            staged_required_bytes += bytes;
            saw_staged = true;
            append_upload_run(runs, static_cast<uint32_t>(byte_start),
                              static_cast<uint32_t>(byte_end), false,
                              kRoutePrecommit, last_access);
            record_direct_write_reject(
                MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                    kMainGpuAccessInFlight,
                bytes);
            continue;
          }
          staged_has_inflight = true;
        }
        staged_required_bytes += bytes;
        saw_staged = true;
        append_upload_run(runs, static_cast<uint32_t>(byte_start),
                          static_cast<uint32_t>(byte_end), false);
        record_direct_write_reject(
            MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                kMainGpuAccessInFlight,
            bytes);
        continue;
      }

      ++trace.pg_completed;
      direct_eligible_bytes += bytes;
      saw_direct = true;
      append_upload_run(runs, static_cast<uint32_t>(byte_start),
                        static_cast<uint32_t>(byte_end), true);
    }

    if (saw_direct && saw_staged) {
      record_direct_write_reject(
          MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
              kMixedRangeSplit,
          uint64_t(end) - start);
    }
  };

  auto direct_write_run = [&](uint32_t start, uint32_t end) {
    uint32_t size = end - start;
    if (ShmemValidFirstEnabled()) {
      // CR-02 (FABLE2_SHMEM_VALID_FIRST, default on): validate, then copy,
      // like the staged path below and the SDK's D3D12/Vulkan uploaders.
      // MakeRangeValid re-arms the page write watch first, so a guest write
      // that lands during the copy faults, invalidates the page and is
      // re-uploaded instead of being lost behind a "valid" page.
      MakeRangeValid(start, size, false);
      copy_guest_bytes(shared_buffer_contents + start, start, size);
      swcache::WriteFence();
    } else {
      copy_guest_bytes(shared_buffer_contents + start, start, size);
      swcache::WriteFence();
      MakeRangeValid(start, size, false);
    }
    if (PreserveTraceEnabled()) {
      // Trace-only race counter (FABLE2_DRAW_SKIP_TRACE=1): a run whose
      // bytes differ from guest RAM while its pages are still marked valid
      // is a CPU write the page watch missed.
      static std::atomic<uint64_t> runs{0}, stale_valid{0};
      const uint64_t n = runs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (std::memcmp(shared_buffer_contents + start, xbox_data + start,
                      size) != 0 &&
          IsRangeValid(start, size)) {
        stale_valid.fetch_add(1, std::memory_order_relaxed);
      }
      if ((n & (n - 1)) == 0 || n % 100000 == 0) {
        REXLOG_INFO(
            "[shmem-valid-first] {}: direct-write runs {}, stale-valid {}",
            ShmemValidFirstEnabled() ? "on" : "off", n,
            stale_valid.load(std::memory_order_relaxed));
      }
    }
    command_processor_.RecordSharedMemoryUploadRoute(
        MetalCommandProcessor::SharedMemoryUploadRoute::kDirectWrite, size);
  };

  auto stage_upload_run = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    uint32_t offset = start;
    uint32_t remaining = end - start;
    while (remaining) {
      if (!blit_encoder) {
        command_processor_.NoteSharedMemoryUploadBreakCause(
            staged_has_current      ? PageAccess::kCurrent
            : staged_has_standalone ? PageAccess::kStandalone
            : staged_has_inflight   ? PageAccess::kInflight
                                    : PageAccess::kCompleted,
            !precommit_allowed || precommit_fell_back);
      }
      MTL::BlitCommandEncoder* encoder = get_blit_encoder();
      if (!encoder) {
        REXLOG_ERROR("MetalSharedMemory::UploadRanges: failed to get blit encoder");
        return false;
      }

      MTL::Buffer* upload_buffer = nullptr;
      size_t upload_offset = 0;
      uint64_t upload_gpu_address = 0;
      size_t upload_size = 0;
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), remaining, page_size,
          &upload_buffer, upload_offset, upload_gpu_address, upload_size);
      if (!upload_mapping || !upload_buffer || !upload_size) {
        REXLOG_ERROR(
            "MetalSharedMemory::UploadRanges: failed to allocate upload "
            "staging buffer");
        return false;
      }
      // RequestPartial rounds the size up to the page alignment. Runs are
      // page-aligned unless FABLE2_MEMEXPORT_PRESERVE carved them, so this
      // clamp is a no-op on the default path.
      upload_size = std::min<size_t>(upload_size, remaining);

      MakeRangeValid(offset, static_cast<uint32_t>(upload_size), false);
      copy_guest_bytes(upload_mapping, offset, upload_size);
      encoder->copyFromBuffer(upload_buffer,
                              static_cast<NS::UInteger>(upload_offset), buffer_,
                              static_cast<NS::UInteger>(offset),
                              static_cast<NS::UInteger>(upload_size));
      // The current command buffer writes these pages: a later pre-commit
      // copy or pre-commit texture load of them in this command buffer
      // would run before this copy, so classify them as current from now on.
      MarkGpuAccess(offset, static_cast<uint32_t>(upload_size),
                    command_processor_.GetCurrentSubmission());
      MarkGpuWrite(offset, static_cast<uint32_t>(upload_size),
                   command_processor_.GetCurrentSubmission(), true);
      command_processor_.RecordSharedMemoryUploadEncoderCopy();
      if (UploadVerifyEnabled()) {
        NoteUploadVerify(offset, static_cast<uint32_t>(upload_size), false);
      }
      command_processor_.RecordSharedMemoryUploadRoute(
          MetalCommandProcessor::SharedMemoryUploadRoute::kStagedBlit,
          upload_size);

      offset += static_cast<uint32_t>(upload_size);
      remaining -= static_cast<uint32_t>(upload_size);
    }
    return true;
  };

  auto precommit_upload_run = [&](const UploadRun& run) -> bool {
    // Only while the render pass that the in-command-buffer blit would end is
    // still open (an earlier in-pass run in this call may have ended it).
    MTL::BlitCommandEncoder* encoder =
        command_processor_.SharedMemoryPrecommitAllowed()
            ? command_processor_.BeginSharedMemoryPrecommit(run.wait)
            : nullptr;
    if (!encoder) {
      staged_has_inflight = true;
      precommit_fell_back = true;
      return stage_upload_run(run.start, run.end);
    }
    uint32_t offset = run.start;
    uint32_t remaining = run.end - run.start;
    while (remaining) {
      MTL::Buffer* upload_buffer = nullptr;
      size_t upload_offset = 0;
      uint64_t upload_gpu_address = 0;
      size_t upload_size = 0;
      // Tagged with the current submission: it completes only after this
      // pre-commit command buffer (it waits on its event before committing).
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), remaining, page_size,
          &upload_buffer, upload_offset, upload_gpu_address, upload_size);
      if (!upload_mapping || !upload_buffer || !upload_size) {
        REXLOG_ERROR(
            "MetalSharedMemory::UploadRanges: failed to allocate pre-commit "
            "staging buffer");
        return false;
      }
      upload_size = std::min<size_t>(upload_size, remaining);
      // Validate before copying (as the staged path and CR-02).
      MakeRangeValid(offset, static_cast<uint32_t>(upload_size), false);
      copy_guest_bytes(upload_mapping, offset, upload_size);
      encoder->copyFromBuffer(upload_buffer,
                              static_cast<NS::UInteger>(upload_offset), buffer_,
                              static_cast<NS::UInteger>(offset),
                              static_cast<NS::UInteger>(upload_size));
      command_processor_.RecordSharedMemoryUploadRoute(
          MetalCommandProcessor::SharedMemoryUploadRoute::kStagedBlit,
          upload_size);
      if (UploadVerifyEnabled()) {
        NoteUploadVerify(offset, static_cast<uint32_t>(upload_size), true);
      }
      MarkPrecommitCopy(offset, static_cast<uint32_t>(upload_size),
                        command_processor_.GetCurrentSubmission());
      offset += static_cast<uint32_t>(upload_size);
      remaining -= static_cast<uint32_t>(upload_size);
    }
    precommit_pages += ((run.end - 1) >> page_size_log2()) -
                       (run.start >> page_size_log2()) + 1;
    return true;
  };

  auto flush_merged_range_impl = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    std::vector<UploadRun> upload_runs;
    build_upload_runs(start, end, upload_runs);
    for (const UploadRun& run : upload_runs) {
      if (run.route == kRoutePrecommit) {
        if (!precommit_upload_run(run)) {
          return false;
        }
      } else if (REXCVAR_GET(metal_shared_memory_direct_write) &&
                 run.direct_safe) {
        direct_write_run(run.start, run.end);
      } else if (!stage_upload_run(run.start, run.end)) {
        return false;
      }
    }
    return true;
  };

  // FABLE2_MEMEXPORT_PRESERVE: carve still-valid memexport output out of the
  // upload. Unset, this is a direct call of the unchanged upload path.
  const bool preserve = MemexportPreserveEnabled() && !preserved_exports_.empty();
  uint64_t preserved_bytes = 0;
  if (preserve) {
    ExpirePreservedExports(PreserveNowUs());
  }
  auto flush_merged_range = [&](uint32_t start, uint32_t end) -> bool {
    if (!preserve || preserved_exports_.empty()) {
      return flush_merged_range_impl(start, end);
    }
    // Collect overlapping exports whose guest bytes are unchanged.
    std::vector<std::pair<uint32_t, uint32_t>> keep;
    for (size_t k = 0; k < preserved_exports_.size();) {
      const PreservedExport& e = preserved_exports_[k];
      if (e.end <= start || e.start >= end) {
        ++k;
        continue;
      }
      if (XXH3_64bits(xbox_data + e.start, e.end - e.start) != e.guest_hash) {
        // The CPU wrote into the export range: its data wins, stop preserving.
        preserved_exports_.erase(preserved_exports_.begin() + k);
        continue;
      }
      keep.emplace_back(std::max(e.start, start), std::min(e.end, end));
      ++k;
    }
    if (keep.empty()) {
      return flush_merged_range_impl(start, end);
    }
    std::sort(keep.begin(), keep.end());
    uint32_t cursor = start;
    for (const auto& kr : keep) {
      if (kr.first > cursor && !flush_merged_range_impl(cursor, kr.first)) {
        return false;
      }
      uint32_t skip_start = std::max(cursor, kr.first);
      if (kr.second > skip_start) {
        // GPU data in the shared buffer is newer than guest RAM here.
        MakeRangeValid(skip_start, kr.second - skip_start, false);
        preserved_bytes += kr.second - skip_start;
      }
      cursor = std::max(cursor, kr.second);
    }
    if (cursor < end) {
      return flush_merged_range_impl(cursor, end);
    }
    return true;
  };

  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    const auto& range = upload_page_ranges[i];
    uint32_t start = range.first * page_size;
    uint32_t end = start + range.second * page_size;
    if (start >= kBufferSize) {
      continue;
    }
    if (end > kBufferSize) {
      end = kBufferSize;
    }

    if (!have_merged) {
      merged_start = start;
      merged_end = end;
      have_merged = true;
      continue;
    }

    // Merge overlapping/adjacent ranges.
    if (start <= merged_end) {
      if (end > merged_end) {
        merged_end = end;
      }
    } else {
      if (!flush_merged_range(merged_start, merged_end)) {
        command_processor_.EndSharedMemoryPrecommit(precommit_pages);
        command_processor_.EndSharedMemoryUploadBlitEncoder(
            MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
                kUploadFailure);
        return false;
      }
      merged_start = start;
      merged_end = end;
    }
  }

  if (have_merged) {
    if (!flush_merged_range(merged_start, merged_end)) {
      command_processor_.EndSharedMemoryPrecommit(precommit_pages);
      command_processor_.EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kUploadFailure);
      return false;
    }
  }

  // Commit the pre-commit copies now, ahead of the (still open) current
  // command buffer, and make the active render pass wait for them.
  command_processor_.EndSharedMemoryPrecommit(precommit_pages);

  if (preserved_bytes && PreserveTraceEnabled()) {
    frame_log::Record("tr_memexport_preserved_bytes", frame_log::CurrentFrame(),
                      preserved_bytes);
  }
  REXLOG_DEBUG("MetalSharedMemory::UploadRanges: Staged {} ranges to Metal buffer",
         num_upload_ranges);
  command_processor_.RecordSharedMemoryDirectWriteEligibility(
      direct_eligible_bytes, staged_required_bytes);

  return true;
}

void MetalSharedMemory::Shutdown() {
  pending_readbacks_.clear();
  upload_buffer_pool_.reset();
  if (buffer_) {
    buffer_->release();
    buffer_ = nullptr;
  }

  ShutdownCommon();  // Base class cleanup
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
