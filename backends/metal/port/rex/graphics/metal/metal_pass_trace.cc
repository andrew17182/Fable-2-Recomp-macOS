// [pass-trace] see metal_pass_trace.h. Trace-only; no behaviour change.
#include "rex/graphics/metal/metal_pass_trace.h"

#include <algorithm>

#include <mach/mach_time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rex/cvar.h"
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/logging.h"

REXCVAR_DECLARE(std::string, log_file);

namespace rex::graphics::metal::pass_trace {
namespace {

constexpr uint32_t kSamplesPerBuffer = 4096;  // 32 KB, the Metal maximum
constexpr size_t kMaxBuffers = 24;

struct Pass {
  char kind = 'R';
  const char* site = "";
  std::string key;
  uint64_t frame = 0;
  uint32_t base = 0;  // first sample index
  uint32_t draws = 0, barriers = 0, fence_waits = 0;
  const char* end_reason = "";
  std::vector<uint32_t> shaders;  // FABLE2_PASS_TRACE_PS
};

struct CbRecord {
  MTL::CounterSampleBuffer* buffer = nullptr;
  uint32_t used = 0;
  std::vector<Pass> passes;
};

struct State {
  std::mutex mutex;
  MTL::Device* device = nullptr;
  MTL::CounterSet* timestamp_set = nullptr;
  std::vector<MTL::CounterSampleBuffer*> free_buffers;
  size_t buffers_created = 0;
  std::unordered_map<MTL::CommandBuffer*, CbRecord*> records;
  // GPU->CPU clock calibration (two pairs, refreshed about once a second).
  double cpu_a = 0, gpu_a = 0, ratio = 1.0;
  uint64_t last_calibration_us = 0;
  // Output.
  std::mutex out_mutex;
  std::vector<std::string> pending;
  FILE* file = nullptr;
  uint64_t dropped_cb = 0;
};
State& Get() {
  static State* s = new State;  // leaked: the runtime hard-exits
  return *s;
}

double MachToNs(double ticks) {
  static const double scale = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return double(t.numer) / double(t.denom);
  }();
  return ticks * scale;
}

bool EnvEnabled() {
  static const bool on = [] {
    const char* t = std::getenv("FABLE2_GPU_TIME_TRACE");
    const char* p = std::getenv("FABLE2_PASS_TRACE");
    return t && t[0] == '1' && !(p && p[0] == '0');
  }();
  return on;
}

void Calibrate(State& s) {
  MTL::Timestamp cpu = 0, gpu = 0;
  s.device->sampleTimestamps(&cpu, &gpu);
  // sampleTimestamps: both values are nanoseconds on Apple silicon (measured);
  // the ratio is still fitted in case a GPU uses another tick.
  const double cpu_ns = double(cpu), gpu_t = double(gpu);
  if (s.gpu_a == 0) {
    s.cpu_a = cpu_ns;  // fixed anchor
    s.gpu_a = gpu_t;
  } else if (gpu_t > s.gpu_a + 1e6) {
    s.ratio = (cpu_ns - s.cpu_a) / (gpu_t - s.gpu_a);
  }
}

// Calibration snapshot, copied under State::mutex (Calibrate updates it on encoding threads
// while completion handlers emit rows).
struct Calibration {
  double cpu_a = 0, gpu_a = 0, ratio = 1.0;
};

// GPU timestamp -> microseconds on the frame-log clock.
double GpuToFrameUs(const Calibration& s, uint64_t gpu) {
  const double cpu_ns = s.cpu_a + (double(gpu) - s.gpu_a) * s.ratio;
  const double uptime_us = cpu_ns / 1000.0;
  // frame_log clock = uptime - origin; derive origin from the current pair.
  const double now_uptime_us = MachToNs(double(mach_absolute_time())) / 1000.0;
  return uptime_us - (now_uptime_us - double(frame_log::NowUs()));
}

char LoadChar(MTL::LoadAction a) {
  switch (a) {
    case MTL::LoadActionDontCare: return 'D';
    case MTL::LoadActionLoad: return 'L';
    case MTL::LoadActionClear: return 'C';
    default: return '?';
  }
}
char StoreChar(MTL::StoreAction a) {
  switch (a) {
    case MTL::StoreActionDontCare: return 'D';
    case MTL::StoreActionStore: return 'S';
    case MTL::StoreActionMultisampleResolve: return 'R';
    case MTL::StoreActionStoreAndMultisampleResolve: return 'B';
    case MTL::StoreActionUnknown: return 'U';
    default: return '?';
  }
}
void AppendAttachment(std::string& key, const char* name,
                      MTL::RenderPassAttachmentDescriptor* a) {
  if (!a) return;
  MTL::Texture* t = a->texture();
  if (!t) return;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s%s:%u/%lux%lux%lu/%c%c%s", key.empty() ? "" : " ", name,
                unsigned(t->pixelFormat()), (unsigned long)t->width(),
                (unsigned long)t->height(), (unsigned long)t->sampleCount(),
                LoadChar(a->loadAction()), StoreChar(a->storeAction()),
                a->resolveTexture() ? "+res" : "");
  key += buf;
}

void OpenFile(State& s) {
  auto main_log = std::filesystem::path(REXCVAR_GET(log_file));
  auto folder = main_log.empty() ? std::filesystem::current_path() / "logs"
                                 : main_log.parent_path();
  if (folder.empty()) folder = std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  auto path = folder / ("passes-" + std::to_string(stamp) + "-" +
                        std::to_string(getpid()) + ".csv");
  s.file = std::fopen(path.c_str(), "wx");
  if (!s.file) {
    REXLOG_WARN("[pass-trace] could not create {}", path.string());
    return;
  }
  std::fputs(
      "frame,kind,site,key,draws,barriers,fence_waits,end_reason,start_us,"
      "vs_us,fs_us,span_us\n",
      s.file);
  std::fflush(s.file);
  REXLOG_INFO("[pass-trace] per-encoder GPU timing -> {}", path.string());
  std::thread([] {
    auto& s = Get();
    std::vector<std::string> batch;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      {
        std::lock_guard<std::mutex> lock(s.out_mutex);
        batch.swap(s.pending);
      }
      for (auto& line : batch) std::fputs(line.c_str(), s.file);
      batch.clear();
      std::fflush(s.file);
    }
  }).detach();
}

void Emit(State& s, CbRecord* rec) {
  if (!rec->buffer || !rec->used) return;
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
  struct Drain { NS::AutoreleasePool* p; ~Drain() { p->release(); } } drain{pool};
  NS::Data* data = rec->buffer->resolveCounterRange(NS::Range(0, rec->used));
  if (!data) return;
  const auto* ts = static_cast<const uint64_t*>(data->mutableBytes());
  const size_t n = data->length() / sizeof(uint64_t);
  Calibration cal;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    cal = {s.cpu_a, s.gpu_a, s.ratio};
  }
  auto valid = [&](uint32_t i) {
    return i < n && ts[i] != 0 && ts[i] != ~uint64_t(0);
  };
  std::vector<std::string> lines;
  lines.reserve(rec->passes.size());
  for (const Pass& p : rec->passes) {
    double vs = -1, fs = -1, span = -1, start = -1;
    uint64_t lo = ~uint64_t(0), hi = 0;
    const uint32_t count = p.kind == 'R' ? 4 : 2;
    for (uint32_t i = 0; i < count; ++i) {
      if (valid(p.base + i)) {
        lo = std::min(lo, ts[p.base + i]);
        hi = std::max(hi, ts[p.base + i]);
      }
    }
    if (p.kind == 'R') {
      if (valid(p.base) && valid(p.base + 1) && ts[p.base + 1] >= ts[p.base])
        vs = double(ts[p.base + 1] - ts[p.base]) * cal.ratio / 1000.0;
      if (valid(p.base + 2) && valid(p.base + 3) && ts[p.base + 3] >= ts[p.base + 2])
        fs = double(ts[p.base + 3] - ts[p.base + 2]) * cal.ratio / 1000.0;
    }
    if (hi >= lo && lo != ~uint64_t(0)) {
      span = double(hi - lo) * cal.ratio / 1000.0;
      start = GpuToFrameUs(cal, lo);
    }
    std::string key = p.key;
    if (!p.shaders.empty()) {
      std::vector<uint32_t> sorted = p.shaders;
      std::sort(sorted.begin(), sorted.end());
      key += " ps:";
      for (size_t i = 0; i < sorted.size(); ++i) {
        char h[12];
        std::snprintf(h, sizeof(h), "%s%08X", i ? "+" : "", sorted[i]);
        key += h;
      }
    }
    char buf[640];
    std::snprintf(buf, sizeof(buf), "%llu,%c,%s,%s,%u,%u,%u,%s,%.1f,%.2f,%.2f,%.2f\n",
                  (unsigned long long)p.frame, p.kind, p.site, key.c_str(), p.draws,
                  p.barriers, p.fence_waits, p.end_reason, start, vs, fs, span);
    lines.emplace_back(buf);
  }
  std::lock_guard<std::mutex> lock(s.out_mutex);
  for (auto& l : lines) s.pending.push_back(std::move(l));
}

// Returns the command buffer's record, creating it (and its sample buffer and completion
// handler) on first use. nullptr when no sample buffer is available. Caller holds s.mutex.
CbRecord* RecordFor(State& s, MTL::CommandBuffer* cmd, uint32_t samples) {
  auto it = s.records.find(cmd);
  CbRecord* rec = nullptr;
  if (it != s.records.end()) {
    rec = it->second;
  } else {
    MTL::CounterSampleBuffer* buffer = nullptr;
    if (!s.free_buffers.empty()) {
      buffer = s.free_buffers.back();
      s.free_buffers.pop_back();
    } else if (s.buffers_created < kMaxBuffers) {
      auto* d = MTL::CounterSampleBufferDescriptor::alloc()->init();
      d->setCounterSet(s.timestamp_set);
      d->setStorageMode(MTL::StorageModeShared);
      d->setSampleCount(kSamplesPerBuffer);
      NS::Error* err = nullptr;
      buffer = s.device->newCounterSampleBuffer(d, &err);
      d->release();
      if (buffer) {
        ++s.buffers_created;
      } else {
        REXLOG_WARN("[pass-trace] newCounterSampleBuffer failed: {}",
                    err ? err->localizedDescription()->utf8String() : "?");
      }
    }
    if (!buffer) {
      ++s.dropped_cb;
      return nullptr;
    }
    rec = new CbRecord;
    rec->buffer = buffer;
    s.records.emplace(cmd, rec);
    const uint64_t now = frame_log::NowUs();
    if (now - s.last_calibration_us > 1000000) {
      s.last_calibration_us = now;
      Calibrate(s);
    }
    cmd->addCompletedHandler([rec, cmd](MTL::CommandBuffer*) {
      auto& s = Get();
      {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.records.erase(cmd);
      }
      Emit(s, rec);
      std::lock_guard<std::mutex> lock(s.mutex);
      s.free_buffers.push_back(rec->buffer);
      delete rec;
    });
  }
  if (rec->used + samples > kSamplesPerBuffer) return nullptr;
  return rec;
}

}  // namespace

bool Enabled() {
  return EnvEnabled() && frame_log::Enabled() && Get().timestamp_set != nullptr;
}

void Initialize(MTL::Device* device) {
  if (!EnvEnabled() || !device) return;
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.device) return;
  if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary)) {
    REXLOG_WARN("[pass-trace] device does not support stage-boundary counter sampling");
    return;
  }
  NS::Array* sets = device->counterSets();
  for (NS::UInteger i = 0; sets && i < sets->count(); ++i) {
    auto* set = sets->object<MTL::CounterSet>(i);
    if (set && set->name() && set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
      s.timestamp_set = set;
      set->retain();
    }
  }
  if (!s.timestamp_set) {
    REXLOG_WARN("[pass-trace] no timestamp counter set");
    return;
  }
  s.device = device;
  Calibrate(s);
  OpenFile(s);
}

PassRef BeginRender(MTL::CommandBuffer* cmd, MTL::RenderPassDescriptor* desc,
                    const char* site) {
  PassRef ref;
  if (!cmd || !desc || !Enabled()) return ref;
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  CbRecord* rec = RecordFor(s, cmd, 4);
  if (!rec) return ref;
  Pass p;
  p.kind = 'R';
  p.site = site;
  p.frame = frame_log::CurrentFrame();
  p.base = rec->used;
  for (uint32_t i = 0; i < 4; ++i) {
    char name[4] = {'c', char('0' + i), 0, 0};
    AppendAttachment(p.key, name, desc->colorAttachments()->object(i));
  }
  AppendAttachment(p.key, "d", desc->depthAttachment());
  if (desc->stencilAttachment() && desc->stencilAttachment()->texture() &&
      (!desc->depthAttachment() ||
       desc->stencilAttachment()->texture() != desc->depthAttachment()->texture())) {
    AppendAttachment(p.key, "s", desc->stencilAttachment());
  } else if (desc->stencilAttachment() && desc->stencilAttachment()->texture()) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), " s/%c%c", LoadChar(desc->stencilAttachment()->loadAction()),
                  StoreChar(desc->stencilAttachment()->storeAction()));
    p.key += buf;
  }
  if (p.key.empty()) p.key = "noattach";
  auto* att = desc->sampleBufferAttachments()->object(0);
  att->setSampleBuffer(rec->buffer);
  att->setStartOfVertexSampleIndex(p.base);
  att->setEndOfVertexSampleIndex(p.base + 1);
  att->setStartOfFragmentSampleIndex(p.base + 2);
  att->setEndOfFragmentSampleIndex(p.base + 3);
  rec->used += 4;
  rec->passes.push_back(std::move(p));
  ref.cb = rec;
  ref.index = int32_t(rec->passes.size() - 1);
  return ref;
}

void ClearRender(MTL::RenderPassDescriptor* desc) {
  if (!desc || !Enabled()) return;
  desc->sampleBufferAttachments()->object(0)->setSampleBuffer(nullptr);
}

void NoteDraw(PassRef pass) {
  if (!pass) return;
  std::lock_guard<std::mutex> lock(Get().mutex);
  ++static_cast<CbRecord*>(pass.cb)->passes[pass.index].draws;
}
bool ShaderKeysEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PASS_TRACE_PS");
    return Enabled() && v && v[0] == '1';
  }();
  return on;
}
void NoteShader(PassRef pass, uint64_t ps) {
  if (!pass || !ps) return;
  std::lock_guard<std::mutex> lock(Get().mutex);
  auto& v = static_cast<CbRecord*>(pass.cb)->passes[pass.index].shaders;
  const uint32_t h = uint32_t(ps >> 32);
  if (v.size() < 6 && std::find(v.begin(), v.end(), h) == v.end()) v.push_back(h);
}
void NoteBarrier(PassRef pass) {
  if (!pass) return;
  std::lock_guard<std::mutex> lock(Get().mutex);
  ++static_cast<CbRecord*>(pass.cb)->passes[pass.index].barriers;
}
void NoteFenceWait(PassRef pass) {
  if (!pass) return;
  std::lock_guard<std::mutex> lock(Get().mutex);
  ++static_cast<CbRecord*>(pass.cb)->passes[pass.index].fence_waits;
}
void EndPass(PassRef pass, const char* end_reason) {
  if (!pass) return;
  std::lock_guard<std::mutex> lock(Get().mutex);
  static_cast<CbRecord*>(pass.cb)->passes[pass.index].end_reason = end_reason;
}

MTL::ComputeCommandEncoder* Compute(MTL::CommandBuffer* cmd, const char* site) {
  return Compute(cmd, site, nullptr);
}

MTL::ComputeCommandEncoder* Compute(MTL::CommandBuffer* cmd, const char* site,
                                    const char* key) {
  if (!cmd) return nullptr;
  if (!Enabled()) return cmd->computeCommandEncoder();
  auto& s = Get();
  std::unique_lock<std::mutex> lock(s.mutex);
  CbRecord* rec = RecordFor(s, cmd, 2);
  if (!rec) {
    lock.unlock();
    return cmd->computeCommandEncoder();
  }
  Pass p;
  p.kind = 'C';
  p.site = site;
  p.key = (key && *key) ? key : site;
  p.frame = frame_log::CurrentFrame();
  p.base = rec->used;
  rec->used += 2;
  rec->passes.push_back(std::move(p));
  auto* d = MTL::ComputePassDescriptor::computePassDescriptor();  // autoreleased
  auto* att = d->sampleBufferAttachments()->object(0);
  att->setSampleBuffer(rec->buffer);
  att->setStartOfEncoderSampleIndex(rec->passes.back().base);
  att->setEndOfEncoderSampleIndex(rec->passes.back().base + 1);
  lock.unlock();
  return cmd->computeCommandEncoder(d);
}

MTL::BlitCommandEncoder* Blit(MTL::CommandBuffer* cmd, const char* site) {
  if (!cmd) return nullptr;
  if (!Enabled()) return cmd->blitCommandEncoder();
  auto& s = Get();
  std::unique_lock<std::mutex> lock(s.mutex);
  CbRecord* rec = RecordFor(s, cmd, 2);
  if (!rec) {
    lock.unlock();
    return cmd->blitCommandEncoder();
  }
  Pass p;
  p.kind = 'B';
  p.site = site;
  p.key = site;
  p.frame = frame_log::CurrentFrame();
  p.base = rec->used;
  rec->used += 2;
  rec->passes.push_back(std::move(p));
  auto* d = MTL::BlitPassDescriptor::blitPassDescriptor();  // autoreleased
  auto* att = d->sampleBufferAttachments()->object(0);
  att->setSampleBuffer(rec->buffer);
  att->setStartOfEncoderSampleIndex(rec->passes.back().base);
  att->setEndOfEncoderSampleIndex(rec->passes.back().base + 1);
  lock.unlock();
  return cmd->blitCommandEncoder(d);
}

}  // namespace rex::graphics::metal::pass_trace
