// Flight recorder C API (docs/telemetry-design-20261008.md, "implemented in phase 1").
//
// The recorder lives in the executable (src/diagnostics/fable2_flight_recorder.*). The Metal
// plugin (librexgpu-metal.dylib) finds it once with dlsym(RTLD_DEFAULT, "fable2_fr_api") and
// calls through this table; the executable exports every global symbol, so the lookup works
// from the plugin (checked with dyld_info -exports / nm -gU on the built binary).
//
// Plain C types only: no SDK, no STL, so both modules can include it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FABLE2_FR_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fable2FrApi {
  uint32_t version;
  uint32_t size;
  // Category level (0 off, 1 ring, 2 stream, 3 detail) by category name ("frame", "gpu", ...).
  int (*level)(const char* category);
  // Register (or look up) an event type: returns its id, 0 when the category is off.
  uint16_t (*type_id)(const char* name, const char* category, const char* arg_a, const char* arg_b,
                      const char* arg_v);
  // One event in the calling thread's ring. t = 0 means now (mach_absolute_time ticks).
  void (*emit)(uint16_t type, uint32_t a, uint32_t b, uint64_t v, uint64_t t);
  // Guest swap: advances the global frame id, feeds the frame-gap / hang detectors.
  uint32_t (*frame_swap)(void);
  // Presenter paint on the UI thread (reported in the hang detail; the hang trigger is the swap stall alone).
  void (*paint)(void);
  // Asynchronous trigger (rate-limited per reason); detail may be NULL.
  void (*trigger)(const char* reason, const char* detail);
  // Synchronous dump: writes a dump now and copies its folder path into out (empty on failure).
  // Returns 1 on success.
  int (*dump_sync)(const char* reason, const char* detail, char* out, size_t out_size,
                   uint32_t timeout_ms);
  // Harness marker (FR "mark" event + text).
  void (*mark)(const char* text);
  // Set a tag from a string "key=value" (value parsed as integer when numeric, else text).
  void (*tag)(const char* key_value);
  // JSON object with tags, frame id, fps, last gap, trace folder. Returns bytes written.
  size_t (*state_json)(char* out, size_t out_size);
  // Heartbeat: hook id from hook_id(name); fired() marks it as having run.
  int (*hook_id)(const char* name);
  void (*fired)(int hook);
  // Guest sampler counters for the old lr-sampler CSV rows (FABLE2_GPU_TIME_TRACE):
  // thread_slot 0 = 3D Engine, 1 = GameThread, 2 = Audio Worker. Returns and resets the counts
  // of samples / samples in a known wait loop since the last call.
  void (*sampler_counts)(int thread_slot, uint32_t* total, uint32_t* wait);
  // Folder of this session's trace/ (empty when the recorder is off).
  const char* (*trace_dir)(void);
  // Current global frame id (guest swaps).
  uint32_t (*frame_id)(void);
} Fable2FrApi;

// Exported by the executable. NULL never; the table's functions are no-ops with FABLE2_TRACE=off.
const Fable2FrApi* fable2_fr_api(void);

#ifdef __cplusplus
}
#endif
