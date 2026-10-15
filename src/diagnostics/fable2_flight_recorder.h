// Flight recorder (telemetry phase 1, docs/telemetry-design-20261008.md).
//
// Always on (FABLE2_TRACE=off disables everything): every thread writes compact 32-byte events into
// its own lock-free ring; a recorder thread samples all guest stacks without suspending anyone,
// watches for triggers (frame gap, hang, first guest AV, AV storm, GPU timeout, persistent miss)
// and writes a dump folder only when one fires or on request:
//   <log folder>/trace/session.json, log-digest.jsonl, [stream.f2t], dumps/NNN-<reason>/
// Read the output with tools/f2diag (python3 tools/f2diag.py report <dump>).
//
// Hot path: Emit() is a thread_local ring pointer, one relaxed load for the frame id and a release
// store of the ring head (no locks, no shared read-modify-write). Category checks are one relaxed
// load of a 64-bit level word.
#pragma once
#include <mach/mach_time.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "fable2_trace_api.h"

namespace fable2::fr {

// Categories (2 bits of level each in g_levels). Keep in sync with kCatNames in the .cpp and
// CATEGORIES in tools/f2diag/f2t.py.
enum Cat : uint8_t {
  kFrame, kGpu, kCpu, kSampler, kWait, kSignal, kIo, kWorker0, kLoad, kUi, kShader, kDraw,
  kClock, kInput, kLog, kMark, kTag, kCrash, kTex, kShmem, kSave, kCatCount
};
static_assert(kCatCount <= 32, "2 bits per category in a 64-bit word");

extern std::atomic<uint64_t> g_levels;
inline int Level(Cat c) { return int((g_levels.load(std::memory_order_relaxed) >> (2 * c)) & 3u); }
inline bool On(Cat c, int lvl = 1) { return Level(c) >= lvl; }
bool Active();  // recorder running (false with FABLE2_TRACE=off or before Init)

struct Event {          // 32 bytes, little-endian
  uint16_t type;        // event type id (header lists name / category / arg names)
  uint8_t thread;       // ring index (header lists host tid / pthread name)
  uint8_t flags;        // Flag bits
  uint32_t frame;       // guest swap id when written
  uint64_t t;           // mach_absolute_time ticks (header: timebase + clock pairs)
  uint32_t a, b;
  uint64_t v;
};
struct Cont {           // continuation record following an event with kHasStack / kHasText
  uint16_t type;        // kTStack or kTText
  uint8_t thread, flags;
  uint8_t n;            // stack: addresses in this record (<= 6); text: bytes (<= 24)
  uint8_t seq;          // index of this continuation after its event
  uint16_t aux;         // log ring: low 16 bits of the event's ring position (validity check)
  uint32_t w[6];        // stack: guest return addresses; text: 24 bytes of UTF-8
};
static_assert(sizeof(Event) == 32 && sizeof(Cont) == 32, "fixed 32-byte records");

enum Flag : uint8_t { kBegin = 1, kEnd = 2, kHasStack = 4, kTorn = 8, kHasText = 16, kSelfTest = 32 };

// Built-in event types. Dynamic types (Metal plugin, frame CSV bridge) start at kTDynamic.
enum Type : uint16_t {
  kTNone = 0, kTStack, kTText, kTSwap, kTGap, kTSample, kTSampleRepeat, kTWait, kTDelay, kTPollWait,
  kTSetEvent, kTLevelLoad, kTWorker0, kTSaveNotice, kTContentClose, kTOpen, kTLog, kTTag, kTMark,
  kTTrigger, kTCpu, kTThreadExit, kTPaint, kTRead, kTLongWait, kTLevelArg, kTBuiltinEnd,
  kTDynamic = 64
};

struct Ring {
  std::atomic<uint64_t> head{0};
  Event* buf = nullptr;
  uint64_t mask = 0;
  uint8_t index = 0;
  uint64_t host_tid = 0;
  uint32_t mach_port = 0;
  char name[48] = {};
  std::atomic<const void*> guest_ctx{nullptr};  // PPCContext* of this guest thread (set by hooks)
  std::atomic<uint64_t> wait_since{0};          // mach ticks when a hooked guest wait began; 0 = none
};

extern thread_local Ring* t_ring;
Ring* RegisterThisThread();  // never null
extern std::atomic<uint32_t> g_frame;

inline uint64_t Now() { return mach_absolute_time(); }
inline Ring* MyRing() {
  Ring* r = t_ring;
  return r ? r : RegisterThisThread();
}

inline void Emit(uint16_t type, uint32_t a, uint32_t b, uint64_t v, uint8_t flags = 0, uint64_t t = 0) {
  Ring* r = MyRing();
  const uint64_t h = r->head.load(std::memory_order_relaxed);
  Event& e = r->buf[h & r->mask];
  e.type = type;
  e.thread = r->index;
  e.flags = flags;
  e.frame = g_frame.load(std::memory_order_relaxed);
  e.t = t ? t : Now();
  e.a = a;
  e.b = b;
  e.v = v;
  r->head.store(h + 1, std::memory_order_release);
}
// An event followed by guest return addresses (6 per continuation record).
void EmitStack(uint16_t type, uint32_t a, uint32_t b, uint64_t v, uint8_t flags, const uint32_t* frames,
               int n, uint64_t t = 0);
// An event followed by text (24 bytes per continuation record, at most 240 bytes).
void EmitText(uint16_t type, uint32_t a, uint32_t b, uint64_t v, const char* text, size_t len,
              uint64_t t = 0);

// Guest wait bookkeeping for the sampler's WAIT state (cheap: two relaxed stores).
inline void WaitBegin(const void* guest_ctx, uint64_t t) {
  Ring* r = MyRing();
  if (!r->guest_ctx.load(std::memory_order_relaxed)) r->guest_ctx.store(guest_ctx, std::memory_order_relaxed);
  r->wait_since.store(t, std::memory_order_relaxed);
}
inline void WaitEnd() { MyRing()->wait_since.store(0, std::memory_order_relaxed); }

// Game-state tags. Each change writes a kTTag event; current values go into every dump,
// session.json and the test-control `state` reply.
enum Tag : uint8_t {
  kTagLevelLoad, kTagLoading, kTagSaveNotice, kTagRegion, kTagMovie, kTagTurbo, kTagFpsMode,
  kTagLevelName, kTagQuest, kTagFocus, kTagSave, kTagSelfTest,
  // Other game instances running beside this one ("0" or "N: playtest 123, agent 456"); pushed by the
  // session watcher through trace/tags-in.txt (tools/fable2_instances.py).
  kTagContention,
  // Readiness (backends/metal/.../metal_readiness.h, docs/readiness.md), set by the Metal plugin while a harness
  // is attached: world drawing (0/1), frame pacing steady (0/1), background shader work pending (count), and the
  // newest guest frame whose frame-CSV rows are on disk.
  kTagWorld, kTagSteady, kTagBgBusy, kTagCsvFrame, kTagCount
};
void SetTag(Tag tag, int64_t value, const char* text = nullptr);
int64_t GetTag(Tag tag);

// Heartbeat: every probe registers once and marks itself fired; session.json and every dump list
// the hooks that never fired (a silently inert probe is visible at once).
int HookId(const char* name);
extern std::atomic<uint8_t> g_hook_fired[128];
void FirstFire(int id);
inline void Fired(int id) {
  if (id >= 0 && id < 128 && !g_hook_fired[id].load(std::memory_order_relaxed)) FirstFire(id);
}
#define FR_HEARTBEAT(name)                                               \
  do {                                                                   \
    static const int fr_hook_id_ = ::fable2::fr::HookId(name);           \
    ::fable2::fr::Fired(fr_hook_id_);                                    \
  } while (0)

// Triggers (asynchronous, rate-limited per reason; dumps run on the recorder thread).
void Trigger(const char* reason, const char* detail = nullptr);

// Swap / paint notifications (from the plugin through the C API).
uint32_t FrameSwap();
void Paint();

// Level-load call bracket (sub_825F3050).
// guest_ctx/sp: the loading thread, whose stack the recorder thread scans for the level name (region tag).
void LevelLoadBegin(uint32_t r3, const void* guest_ctx = nullptr, uint32_t sp = 0);
void LevelLoadEnd(uint64_t dur_ticks);
// Region tag: scan a guest thread's stack for a level name on the recorder thread (purpose 0 = the
// level-load call; the only one used).
void RequestRegionScan(const void* guest_ctx, uint32_t sp, int purpose);

// Lifecycle (Fable2App).
void Init();                 // OnPostInitLogging: parse FABLE2_TRACE, create trace/, start recorder
void InstallLogSink();       // OnPostInitLogging, after Init
void OnPostSetup();          // tags that need the runtime (fps mode, turbo)
void OnCloseRequested(const char* why);  // synchronous "exit" dump (rate-limited to one)
uint64_t TicksPerMs();
double TicksToMs(uint64_t ticks);

// True when s is a level name from data/miscellaneous/levelconnectiongraph.txt (empty list: any).
bool KnownLevelName(const char* s);

// Legacy log-line windows (FABLE2_TRACE_LOG_SECONDS; default unlimited).
bool LogWindowOpen();

}  // namespace fable2::fr
