#ifdef FABLE2_CONTINUATION_NATIVE_TRACE
#include "core/continuation/native_import_trace_api.h"
#endif
#pragma once
// Opt-in observational thread-root trace for continuation integration.
// Forward the original import without changing guest thread/bootstrap behavior.
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach/mach.h>
#include <time.h>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
#include <array>

namespace fable2::thread_trace {
using Import = void (*)(PPCContext&, uint8_t*);
inline uint64_t Now() {
  timespec value{};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return uint64_t(value.tv_sec) * 1000000000ull + value.tv_nsec;
}
inline uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t value = 0;
  mach_vm_size_t actual = 0;
  if (!address || mach_vm_read_overwrite(mach_task_self(),
      reinterpret_cast<mach_vm_address_t>(base + address), sizeof(value),
      reinterpret_cast<mach_vm_address_t>(&value), &actual) != KERN_SUCCESS ||
      actual != sizeof(value)) return 0;
  return __builtin_bswap32(value);
}
// getenv on every hooked call showed up on guest UI threads (__findenv_locked, perf audit
// 2026-10-11 candidate 14). The environment does not change after launch, so read it once;
// FABLE2_PROBE_GETENV_CACHE=0 reads it on every call as before. index 0 = FABLE2_THREAD_TRACE,
// 1 = FABLE2_TEST_CONTROL_DIR.
inline const char* CachedEnv(int index) {
  static const bool cache = [] {
    const char* v = std::getenv("FABLE2_PROBE_GETENV_CACHE");
    return !(v && v[0] == '0');
  }();
  static const char* const names[2] = {"FABLE2_THREAD_TRACE", "FABLE2_TEST_CONTROL_DIR"};
  if (!cache) return std::getenv(names[index]);
  static const char* const values[2] = {std::getenv(names[0]), std::getenv(names[1])};
  return values[index];
}
inline void Forward(Import original, PPCContext& c, uint8_t* base) {
  if (!original) std::abort();
  const char* enabled = fable2::thread_trace::CachedEnv(0);
  const char* directory = fable2::thread_trace::CachedEnv(1);
  if (!enabled || std::string(enabled) != "1" || !directory) {
    #ifdef FABLE2_CONTINUATION_NATIVE_TRACE
  fable2::continuation::native_trace::InvokeImport(0x832B2B5C,"__imp__ExCreateThread",original,c,base);
  #else
  original(c, base);
  #endif
    return;
  }
  uint32_t args[7] = {uint32_t(c.r3.u64), uint32_t(c.r4.u64), uint32_t(c.r5.u64),
      uint32_t(c.r6.u64), uint32_t(c.r7.u64), uint32_t(c.r8.u64), uint32_t(c.r9.u64)};
  const uint32_t caller = uint32_t(c.lr);
  // This observed thread root calls vtable slot 1. Record the target while
  // its creation context is still available; do not invoke or modify it.
  const uint32_t vtable = args[4] == 0x82B38598 ? Read32(base, args[5]) : 0;
  const uint32_t virtual_target = vtable ? Read32(base, vtable + 4) : 0;
  // PPC CRT callback dispatcher 0x82CC16C0 walks this sentinel list.
  // Bounded checked reads are observational; a concurrent mutation may make
  // the list incomplete, so this does not certify callbacks automatically.
  constexpr uint32_t crt_head = 0x832EFEBC;
  std::vector<uint32_t> crt_nodes,crt_callbacks;
  for(uint32_t node=Read32(base,crt_head);node && node!=crt_head && crt_nodes.size()<64;
      node=Read32(base,node)) {
    if(std::find(crt_nodes.begin(),crt_nodes.end(),node)!=crt_nodes.end())break;
    crt_nodes.push_back(node);
    crt_callbacks.push_back(Read32(base,node+8));
  }
  const uint64_t begin = Now();
#ifdef FABLE2_CONTINUATION_NATIVE_TRACE
  fable2::continuation::native_trace::InvokeImport(0x832B2B5C,"__imp__ExCreateThread",original,c,base);
#else
  original(c, base);
#endif
  const uint64_t end = Now();
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::ofstream file(std::string(directory) + "/thread-starts.jsonl", std::ios::app);
  file << "{\"begin_ns\":" << begin << ",\"end_ns\":" << end
       << ",\"caller_lr\":" << caller << ",\"stack_size\":" << args[1]
       << ",\"xapi_startup\":" << args[3] << ",\"start_address\":" << args[4]
       << ",\"start_context\":" << args[5] << ",\"flags\":" << args[6]
       << ",\"virtual_target\":" << virtual_target
       << ",\"periodic_class\":" << (virtual_target==0x82C08310?Read32(base,args[5]+20):0)
       << ",\"crt_attach\":" << Read32(base,0x82010CBC)
       << ",\"crt_detach\":" << Read32(base,0x82010CC0)
       << ",\"status\":" << uint32_t(c.r3.u64)
       << ",\"handle\":" << Read32(base, args[0])
       << ",\"thread_id\":" << Read32(base, args[2]) << ",\"crt_callbacks\":[";
  for(size_t index=0;index<crt_callbacks.size();++index) {
    if(index)file<<',';
    file<<crt_callbacks[index];
  }
  file<<"]}\n";
}
}
extern "C" void __imp__ExCreateThread(PPCContext& c, uint8_t* base) {
  static auto original = reinterpret_cast<fable2::thread_trace::Import>(
      dlsym(RTLD_NEXT, "__imp__ExCreateThread"));
  fable2::thread_trace::Forward(original, c, base);
}
// Queue callbacks are observed after the original pop returns. This hook never
// runs the job or changes its ownership; observing a target is not certification.
extern "C" void ProcessAndProcessAndProcess573_82B458C0(PPCContext& c,uint8_t* base) {
  __imp__ProcessAndProcessAndProcess573_82B458C0(c,base);
  const char* enabled=fable2::thread_trace::CachedEnv(0);
  const char* directory=fable2::thread_trace::CachedEnv(1);
  if(!enabled || std::string(enabled)!="1" || !directory || !c.r3.u32)return;
  // Only this worker assigns the observed values the job interface meaning.
  if(uint32_t(c.lr)!=0x82B45588 && uint32_t(c.lr)!=0x82B455C4)return;
  const uint32_t job=c.r3.u32;
  const uint32_t table=fable2::thread_trace::Read32(base,job);
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::ofstream file(std::string(directory)+"/queued-job-targets.jsonl",std::ios::app);
  file<<"{\"time_ns\":"<<fable2::thread_trace::Now()
      <<",\"caller_lr\":"<<uint32_t(c.lr)<<",\"job\":"<<job
      <<",\"execute\":"<<fable2::thread_trace::Read32(base,table+28)
      <<",\"release\":"<<fable2::thread_trace::Read32(base,table+8)<<"}\n";
}
// Batch the remaining periodic task indirection without changing dispatch.
extern "C" void VirtualCallWithCheck3_82C55310(PPCContext& c,uint8_t* base) {
  const char* enabled=fable2::thread_trace::CachedEnv(0);
  const char* directory=fable2::thread_trace::CachedEnv(1);
  if(enabled && std::string(enabled)=="1" && directory && uint32_t(c.lr)==0x82C083E4) {
    auto argument=fable2::thread_trace::Read32(base,c.r3.u32+4);
    auto task=fable2::thread_trace::Read32(base,c.r3.u32+8);
    auto index=fable2::thread_trace::Read32(base,c.r29.u32+20);
    auto next=task==0x82C553D0?fable2::thread_trace::Read32(base,fable2::thread_trace::Read32(base,argument)+104):0;
    static std::mutex mutex;static std::set<std::array<uint32_t,3>> seen;
    std::lock_guard lock(mutex);
    if(seen.insert({index,task,next}).second) {
      std::ofstream file(std::string(directory)+"/periodic-task-targets.jsonl",std::ios::app);
      file<<"{\"class\":"<<index<<",\"task\":"<<task<<",\"next\":"<<next<<"}\n";
    }
  }
  __imp__VirtualCallWithCheck3_82C55310(c,base);
}
// Observe all actual periodic UI task registrations in one native startup run.
inline void RecordPeriodicUiTask(PPCContext& c,uint8_t* base) {
  const char* enabled=fable2::thread_trace::CachedEnv(0);
  const char* directory=fable2::thread_trace::CachedEnv(1);
  if(enabled && std::string(enabled)=="1" && directory && uint32_t(c.lr)==0x82C083E4) {
    auto task=fable2::thread_trace::Read32(base,c.r3.u32+4);
    auto index=fable2::thread_trace::Read32(base,c.r29.u32+20);
    static std::mutex mutex;static std::set<std::array<uint32_t,2>> seen;
    std::lock_guard lock(mutex);
    if(seen.insert({index,task}).second) {
      std::ofstream file(std::string(directory)+"/periodic-ui-targets.jsonl",std::ios::app);
      file<<"{\"class\":"<<index<<",\"task\":"<<task<<"}\n";
    }
  }
}
#endif
