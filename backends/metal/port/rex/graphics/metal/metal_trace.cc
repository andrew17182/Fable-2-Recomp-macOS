#include "rex/graphics/metal/metal_trace.h"

#include <dlfcn.h>
#include <mach/mach_time.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "rex/logging.h"

namespace rex::graphics::metal::trace {
namespace {
const Fable2FrApi* Resolve() {
  using Fn = const Fable2FrApi* (*)();
  auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "fable2_fr_api"));
  const Fable2FrApi* api = fn ? fn() : nullptr;
  if (api && (api->version != FABLE2_FR_API_VERSION || api->size < sizeof(Fable2FrApi))) {
    REXLOG_WARN("[trace] flight recorder API version {} (size {}) does not match the plugin ({}); not used",
                api->version, api->size, FABLE2_FR_API_VERSION);
    api = nullptr;
  }
  REXLOG_INFO("[trace] Metal plugin: flight recorder API {} (dlsym fable2_fr_api from the executable)",
              api ? "found" : "not found");
  return api;
}
const char* CategoryFor(const char* name) {
  auto starts = [name](const char* p) { return std::strncmp(name, p, std::strlen(p)) == 0; };
  if (starts("tr_tex_")) return "tex";
  if (starts("ds_")) return "draw";
  if (starts("tcpu_")) return "cpu";
  if (starts("smp_")) return "sampler";
  if (starts("tr_n_")) return "shmem";
  if (starts("gpu_") || starts("tr_sync") || starts("tr_slot") || starts("tr_cp")) return "gpu";
  return "frame";
}
}  // namespace

const Fable2FrApi* Api() {
  static const Fable2FrApi* api = Resolve();
  return api;
}

uint32_t Swap() {
  const Fable2FrApi* api = Api();
  if (!api) return 0;
  static const int hook = api->hook_id("metal_IssueSwap");
  api->fired(hook);
  return api->frame_swap();
}

void Paint() {
  const Fable2FrApi* api = Api();
  if (!api) return;
  static const int hook = api->hook_id("metal_PaintAndPresent");
  api->fired(hook);
  api->paint();
}

void Csv(const char* name, uint64_t frame, uint64_t mach_ticks, uint64_t duration_us) {
  const Fable2FrApi* api = Api();
  if (!api || !name) return;
  static std::mutex mutex;
  static std::unordered_map<const char*, uint16_t> types;  // names are string literals
  uint16_t type;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = types.find(name);
    if (it == types.end()) {
      it = types.emplace(name, api->type_id(name, CategoryFor(name), "frame", "", "duration_us")).first;
    }
    type = it->second;
  }
  if (type) api->emit(type, uint32_t(frame), 0, duration_us, mach_ticks);
}

uint64_t NowTicks() { return mach_absolute_time(); }

uint64_t UptimeSecondsToTicks(double seconds) {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
  }();
  return seconds > 0 ? uint64_t(seconds * 1e9 * tb.denom / tb.numer) : 0;
}
}  // namespace rex::graphics::metal::trace
