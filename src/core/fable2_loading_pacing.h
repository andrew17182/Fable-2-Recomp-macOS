// Isolate the front-end audio-bank preload throttle from general guest sleeps.
// Original PPC at82354ABC returns from the bank loader, then delays400ms
// (alertable) before advancing to the next bank. The enclosing guest worker
// publishes its completion flag only after walking the entire original list.
// This override retains every loader invocation and the original delay helper;
// only this one call site's duration is reduced. An environment override of400
// restores the original pacing for comparison.
#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <mach/mach_vm.h>
#include <rex/logging/macros.h>
#include "fable2_flight_recorder.h"
#include "fable2_session_journal.h"
#include "fable2_guest_stack.h"
#include "fable2_fast_pacing.h"

namespace fable2::loading {
inline int AudioPreloadDelayMs() {
  static const int duration=[] {
    const char* value=std::getenv("FABLE2_AUDIO_PRELOAD_DELAY_MS");
    if(!value || !*value)return 10;
    char* end=nullptr;long parsed=std::strtol(value,&end,10);
    return end && !*end && parsed>=1 && parsed<=400 ? int(parsed) : 10;
  }();
  return duration;
}
void StartProfiler();
// [loading-pacing] log lines: load:3 in the flight recorder's FABLE2_TRACE (FABLE2_LOADING_TRACE=1 is
// an alias for load:3,wait:3,...). The recorder parses the switch in OnPostInitLogging, before any
// guest thread runs, so this cached check never sees an uninitialised level word.
inline bool Trace() {
  static const bool on=[] {
    const bool on=fable2::fr::On(fable2::fr::kLoad,3);
    if(on)StartProfiler();
    return on;
  }();
  return on;
}
// Guest stack (up to n return addresses, the import's LR first) for recorder events.
inline int StackOf(PPCContext& ctx,uint8_t* base,uint32_t* out,int n,uint32_t lr) {
  out[0]=lr;
  return 1+fable2::gstack::OwnStack(base,ctx.r1.u32,out+1,n-1);
}
inline int StackOf(PPCContext& ctx,uint8_t* base,uint32_t* out,int n) {return StackOf(ctx,base,out,n,uint32_t(ctx.lr));}
}

// Load profiler (log lines with load:3 / FABLE2_LOADING_TRACE=1). Every 100 ms (for
// FABLE2_TRACE_LOG_SECONDS, default unlimited; it was the first 30 s) one "[loading-pacing] prof" line
// lists, per host thread with activity: CPU ms (mach thread_info), ms blocked in guest
// kernel waits/sleeps of any length (KeWaitForSingleObject, NtWaitForSingleObjectEx,
// KeWaitForMultipleObjects, NtWaitForMultipleObjectsEx, KeDelayExecutionThread imports),
// and ms/count/bytes in NtReadFile calls. The import hooks forward through
// dlsym(RTLD_NEXT) like the skip-intro NtCreateFile hook.
#include <mach/mach.h>
#include <pthread.h>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
namespace fable2::loading::prof {
struct Acc {uint64_t tid=0;std::atomic<uint64_t> wait_ns{0},wait_n{0},read_ns{0},read_n{0},read_bytes{0};};
inline std::mutex& Mu() {static std::mutex m;return m;}
inline std::vector<Acc*>& All() {static std::vector<Acc*> v;return v;}
inline Acc& Mine() {
  thread_local Acc* acc=[] {
    auto* a=new Acc();pthread_threadid_np(nullptr,&a->tid);
    std::lock_guard<std::mutex> lock(Mu());All().push_back(a);return a;
  }();
  return *acc;
}
inline uint64_t NowNs() {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
using Import=void(*)(PPCContext&,uint8_t*);
// Every guest kernel wait/sleep import: the flight recorder marks the thread WAIT for the sampler
// and records waits >= 2 ms (wait:1) with a 4-frame stack, >= 50 ms with 14 frames.
inline void TimedWait(Import real,PPCContext& ctx,uint8_t* base,const char* sym="") {
  if(!fable2::fr::On(fable2::fr::kWait)){real(ctx,base);return;}
  const uint32_t lr=uint32_t(ctx.lr);
  const uint64_t m0=mach_absolute_time();
  fable2::fr::WaitBegin(&ctx,m0);
  real(ctx,base);
  const uint64_t m1=mach_absolute_time();
  fable2::fr::WaitEnd();
  const uint64_t dm=m1-m0;
  static const uint64_t two_ms=2*fable2::fr::TicksPerMs(),fifty_ms=50*fable2::fr::TicksPerMs(),one_s=1000*fable2::fr::TicksPerMs();
  if(dm>=two_ms || fable2::fr::On(fable2::fr::kWait,3)) {
    uint32_t st[15];
    const int n=fable2::loading::StackOf(ctx,base,st,dm>=fifty_ms?15:5,lr);
    fable2::fr::EmitStack(dm>=one_s?fable2::fr::kTLongWait:fable2::fr::kTWait,lr,ctx.r3.u32,dm,0,st,n,m1);
  }
  if(!fable2::loading::Trace())return;
  const uint64_t dt=uint64_t(fable2::fr::TicksToMs(dm)*1e6);
  Acc& a=Mine();a.wait_ns.fetch_add(dt,std::memory_order_relaxed);a.wait_n.fetch_add(1,std::memory_order_relaxed);
  // Any kernel wait/sleep import that blocked >= 1 s (whole session; a 5 s stall shows here).
  if(dt>=1000000000ull) {
    char name[64]="";pthread_getname_np(pthread_self(),name,sizeof name);
    REXSYS_INFO("[loading-pacing] long kernel wait {} {:.1f}ms result={:#x} thread '{}' caller {:08X} stack {}",sym,dt/1e6,ctx.r3.u32,name,lr,
        fable2::skipintro::guest_stack(ctx,base));
  }
}
inline void Sampler() {
  pthread_setname_np("LoadProfiler");
  struct Prev {uint64_t cpu=0,wait=0,read=0,readn=0,bytes=0;};
  std::unordered_map<uint64_t,Prev> prev;
  const auto start=std::chrono::steady_clock::now();
  while(fable2::fr::LogWindowOpen()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::unordered_map<uint64_t,Acc*> accs;
    {std::lock_guard<std::mutex> lock(Mu());for(Acc* a:All())accs[a->tid]=a;}
    thread_act_array_t threads=nullptr;mach_msg_type_number_t count=0;
    if(task_threads(mach_task_self(),&threads,&count)!=KERN_SUCCESS)continue;
    std::string line;
    for(mach_msg_type_number_t i=0;i<count;++i) {
      thread_identifier_info_data_t id{};mach_msg_type_number_t n=THREAD_IDENTIFIER_INFO_COUNT;
      thread_basic_info_data_t bi{};mach_msg_type_number_t nb=THREAD_BASIC_INFO_COUNT;
      if(thread_info(threads[i],THREAD_IDENTIFIER_INFO,(thread_info_t)&id,&n)==KERN_SUCCESS &&
         thread_info(threads[i],THREAD_BASIC_INFO,(thread_info_t)&bi,&nb)==KERN_SUCCESS) {
        const uint64_t cpu=(uint64_t(bi.user_time.seconds)+bi.system_time.seconds)*1000000000ull+
            (uint64_t(bi.user_time.microseconds)+bi.system_time.microseconds)*1000ull;
        Prev now{cpu,0,0,0,0};
        if(auto it=accs.find(id.thread_id);it!=accs.end()) {
          now.wait=it->second->wait_ns.load();now.read=it->second->read_ns.load();
          now.readn=it->second->read_n.load();now.bytes=it->second->read_bytes.load();
        }
        Prev& p=prev[id.thread_id];
        const double dc=(now.cpu-p.cpu)/1e6,dw=(now.wait-p.wait)/1e6,dr=(now.read-p.read)/1e6;
        if(dc>=1.0 || dw>=1.0 || dr>=0.5 || now.readn!=p.readn) {
          char name[64]="";
          if(pthread_t pt=pthread_from_mach_thread_np(threads[i]))pthread_getname_np(pt,name,sizeof name);
          char buf[200];
          std::snprintf(buf,sizeof buf," | t%llu %s cpu=%.1f wait=%.1f read=%.1f/%llu/%llu",(unsigned long long)id.thread_id,
              name[0]?name:"?",dc,dw,dr,(unsigned long long)(now.readn-p.readn),(unsigned long long)(now.bytes-p.bytes));
          line+=buf;
        }
        p=now;
      }
      mach_port_deallocate(mach_task_self(),threads[i]);
    }
    vm_deallocate(mach_task_self(),(vm_address_t)threads,count*sizeof(thread_act_t));
    REXSYS_INFO("[loading-pacing] prof t={:.3f}{}",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),line);
  }
}
}
inline void fable2::loading::StartProfiler() {std::thread(prof::Sampler).detach();}
#define FABLE2_PROF_WAIT_HOOK(sym) \
  extern "C" void __imp__##sym(PPCContext& ctx,uint8_t* base) { \
    static const auto real=reinterpret_cast<fable2::loading::prof::Import>(dlsym(RTLD_NEXT,"__imp__" #sym)); \
    FR_HEARTBEAT(#sym); \
    fable2::loading::prof::TimedWait(real,ctx,base,#sym); \
  }
FABLE2_PROF_WAIT_HOOK(KeWaitForSingleObject)
FABLE2_PROF_WAIT_HOOK(NtWaitForSingleObjectEx)
FABLE2_PROF_WAIT_HOOK(KeWaitForMultipleObjects)
FABLE2_PROF_WAIT_HOOK(NtWaitForMultipleObjectsEx)
FABLE2_PROF_WAIT_HOOK(KeDelayExecutionThread)
#undef FABLE2_PROF_WAIT_HOOK
// NtReadFile(handle r3, event r4, apc r5, apc ctx r6, io_status r7, buffer r8, length r9, offset r10)
extern "C" void __imp__NtReadFile(PPCContext& ctx,uint8_t* base) {
  static const auto real=reinterpret_cast<fable2::loading::prof::Import>(dlsym(RTLD_NEXT,"__imp__NtReadFile"));
  FR_HEARTBEAT("NtReadFile");
  const bool io2=fable2::fr::On(fable2::fr::kIo,2);
  if(!fable2::loading::Trace() && !io2){real(ctx,base);return;}
  const uint32_t length=ctx.r9.u32,handle=ctx.r3.u32;
  const uint64_t m0=mach_absolute_time();
  const uint64_t t0=fable2::loading::prof::NowNs();real(ctx,base);
  if(io2)fable2::fr::Emit(fable2::fr::kTRead,handle,length,mach_absolute_time()-m0);
  if(!fable2::loading::Trace())return;
  auto& a=fable2::loading::prof::Mine();
  a.read_ns.fetch_add(fable2::loading::prof::NowNs()-t0,std::memory_order_relaxed);
  a.read_n.fetch_add(1,std::memory_order_relaxed);a.read_bytes.fetch_add(length,std::memory_order_relaxed);
}
extern "C" void __imp__ByteValueCheck_82CBC678(PPCContext&,uint8_t*);
extern "C" void ByteValueCheck_82CBC678(PPCContext& ctx,uint8_t* base) {
  const bool preload=uint32_t(ctx.lr)==0x82354AC8 && ctx.r3.u32==400 && ctx.r4.u8==1;
  if(!preload){__imp__ByteValueCheck_82CBC678(ctx,base);return;}
  const int duration=fable2::loading::AudioPreloadDelayMs();
  const auto start=std::chrono::steady_clock::now();
  ctx.r3.s64=duration;
  __imp__ByteValueCheck_82CBC678(ctx,base);
  if(fable2::loading::Trace()) {
    REXSYS_INFO("[loading-pacing] audio bank throttle requested={}ms actual={:.2f}ms result={:#x}",
        duration,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),ctx.r3.u32);
  }
}

// Guest timed sleeps (DelayMilliseconds_82CC2028(r3=ms, r4=alertable), the title's only caller
// of KeDelayExecutionThread): the flight recorder records sleeps of >= 2 ms (wait:1) with requested
// vs actual duration and a 4-frame stack. With load:3 (FABLE2_LOADING_TRACE=1) each also gets a
// "[loading-pacing] delay" line (no 30 s window or 6000-line cap any more; FABLE2_TRACE_LOG_SECONDS
// limits it).
extern "C" void __imp__DelayMilliseconds_82CC2028(PPCContext&,uint8_t*);
extern "C" void DelayMilliseconds_82CC2028(PPCContext& ctx,uint8_t* base) {
  FR_HEARTBEAT("DelayMilliseconds_82CC2028");
  const uint32_t ms=ctx.r3.u32;
  if(!fable2::fr::On(fable2::fr::kWait) || ms<2 || ms==0xFFFFFFFFu){__imp__DelayMilliseconds_82CC2028(ctx,base);return;}
  const uint32_t lr=uint32_t(ctx.lr);
  uint32_t st[5];
  const int n=fable2::loading::StackOf(ctx,base,st,5);
  const bool log=fable2::loading::Trace() && fable2::fr::LogWindowOpen();
  const std::string stack=log ? fable2::skipintro::guest_stack(ctx,base) : std::string();
  const auto start=std::chrono::steady_clock::now();
  const uint64_t m0=mach_absolute_time();
  __imp__DelayMilliseconds_82CC2028(ctx,base);
  const uint64_t m1=mach_absolute_time();
  fable2::fr::EmitStack(fable2::fr::kTDelay,lr,ms,m1-m0,0,st,n,m1);
  if(log) {
    REXSYS_INFO("[loading-pacing] delay requested={}ms actual={:.2f}ms stack {}",ms,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),stack);
  }
}

// Guest waits through the WaitForSingleObjectEx wrapper at 822D74A0 (r3=handle, r4=timeout ms,
// r5=alertable). The kernel import underneath is recorded by TimedWait; this adds the handle,
// timeout and result as a recorder "wait" event for waits >= 5 ms. With load:3 the old
// "[loading-pacing] wait" / "long wait" lines (windows and caps removed).
extern "C" void __imp__KeWaitForSingleObject_Guest(PPCContext&,uint8_t*);
extern "C" void KeWaitForSingleObject_Guest(PPCContext& ctx,uint8_t* base) {
  FR_HEARTBEAT("KeWaitForSingleObject_Guest");
  if(!fable2::fr::On(fable2::fr::kWait)){__imp__KeWaitForSingleObject_Guest(ctx,base);return;}
  const uint32_t ms=ctx.r4.u32,handle=ctx.r3.u32,lr0=uint32_t(ctx.lr);
  const bool trace=fable2::loading::Trace() && fable2::fr::LogWindowOpen();
  const bool candidate=trace && ms>=2;
  // Long finite waits (>=1 s): a timeout there is a lost signal.
  const bool long_wait=!candidate && trace && ms>=1000 && ms!=0xFFFFFFFFu;
  const std::string stack=candidate||long_wait ? fable2::skipintro::guest_stack(ctx,base) : std::string();
  const auto start=std::chrono::steady_clock::now();
  const uint64_t m0=mach_absolute_time();
  __imp__KeWaitForSingleObject_Guest(ctx,base);
  const uint64_t dm=mach_absolute_time()-m0;
  static const uint64_t five_ms=5*fable2::fr::TicksPerMs();
  if(dm>=five_ms) {
    // a = handle, b = result, v = duration; the stack is the caller chain (timeout in the text-free b high bits is lost:
    // use the TimedWait event of the same thread for the import side).
    uint32_t st[15];
    const int n=fable2::loading::StackOf(ctx,base,st,15,lr0);  // r1 is restored after the call
    fable2::fr::EmitStack(fable2::fr::kTWait,handle,ctx.r3.u32,dm,fable2::fr::kEnd,st,n);
  }
  if(long_wait) {
    const double actual=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(actual>=200.0) {
      REXSYS_INFO("[loading-pacing] long wait handle={:#x} timeout={}ms actual={:.2f}ms result={:#x} stack {}",
          handle,ms,actual,ctx.r3.u32,stack);
    }
    return;
  }
  if(!candidate)return;
  const double actual=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  if(actual<5.0)return;
  REXSYS_INFO("[loading-pacing] wait handle={:#x} timeout={}ms actual={:.2f}ms result={:#x} stack {}",
      handle,ms,actual,ctx.r3.u32,stack);
}

// Boot loader I/O drain (default on; FABLE2_BOOT_LOADER_KICK=0 restores original pacing).
// The boot loader thread (ProcessAndProcessAndProcess211_822F33B8) loads ~37 audio
// databases (.adb) one by one; each file open is marshalled to job worker 0 through
// ProcessAndCreateEventAndProcess_82BFF460: post a closure to queue 83334A18, then
// wait (ProcessAndWaitMapStatus_82B49B50, return 82BFF4F4) on a private event.
// Worker 0 (struct 83334BFC: +20 index 0, +24 period 50 ms) polls every 1 ms but runs
// its task list only every 50 ms of guest time, so each open costs ~50 ms: ~1.9 s of
// the white screen after the logo movie. The queue's drain task is
// ProcessAndProcessAndProcess1552_82BFD7F0 (ignores r3). When the boot loader is about
// to wait, this flags a drain; worker 0's next 1 ms poll (CheckTimeoutCondition_82C08010,
// on worker 0's own thread) runs only that drain task once. Worker 0's other tasks and
// its 50 ms cadence are unchanged. The caller test walks the guest back chain and stops
// at the first non-code return address (following the outermost frame's back chain
// faults on the unmapped stack top).
namespace fable2::loading {
inline bool BootLoaderKick() {
  static const bool on=[] {const char* v=std::getenv("FABLE2_BOOT_LOADER_KICK");return !(v && *v=='0');}();
  return on;
}
inline std::atomic<bool>& DrainPending() {static std::atomic<bool> pending{false};return pending;}
// Same decisions as the original loop: 24 frames, stop at a bad back chain or the first non-code
// return address (the outermost frame's chain points at the unmapped stack top; never followed).
inline bool InBootLoader(PPCContext& ctx,uint8_t* base) {
  bool found=false;
  fable2::gstack::WalkBackChain(ctx.r1.u32,24,fable2::gstack::Range{},
      [base](uint32_t addr,uint32_t* v){*v=fable2::gstack::LoadOwn(base,addr);return true;},
      [&found](uint32_t lr){
        if(lr>=0x822F33B8u && lr<0x822F3638u){found=true;return false;}
        return true;
      });
  return found;
}
}
// Extension (default on; FABLE2_WORKER0_DRAIN=0 limits the drain to the boot loader again): the
// same drain for the GameThread's marshalled ops during save loads and area transitions
// (17 x ~50 ms per transition). Marshal helpers and their wait return addresses:
// 82BFE240, 82BFE30C, 82BFE408 (82BFE2xx-82BFE4xx siblings) and 82BFF4F4 (file open).
namespace fable2::loading {
inline bool Worker0DrainAll() {
  static const bool on=[] {const char* v=std::getenv("FABLE2_WORKER0_DRAIN");return !(v && *v=='0');}();
  return on;
}
inline bool OnGameThread() {
  thread_local const bool game=[] {
    char name[64]="";pthread_getname_np(pthread_self(),name,sizeof name);
    return std::strncmp(name,"GameThread",10)==0;
  }();
  return game;
}
inline bool IsMarshalWait(uint32_t lr) {
  return lr==0x82BFF4F4u || lr==0x82BFE240u || lr==0x82BFE30Cu || lr==0x82BFE408u;
}
}
extern "C" void __imp__ProcessAndWaitMapStatus_82B49B50(PPCContext&,uint8_t*);
extern "C" void ProcessAndWaitMapStatus_82B49B50(PPCContext& ctx,uint8_t* base) {
  FR_HEARTBEAT("ProcessAndWaitMapStatus_82B49B50");
  const uint32_t lr=uint32_t(ctx.lr);
  if(fable2::loading::BootLoaderKick()) {
    const bool kick=(lr==0x82BFF4F4 && fable2::loading::InBootLoader(ctx,base)) ||
        (fable2::loading::Worker0DrainAll() && fable2::loading::IsMarshalWait(lr) && fable2::loading::OnGameThread());
    if(kick)fable2::loading::DrainPending().store(true,std::memory_order_release);
  }
  // Trace-only: every wait for a closure marshalled to job worker 0 (queue 83334A18):
  // 82BFF4F4 (ProcessAndCreateEventAndProcess_82BFF460, file open) and the sibling
  // marshal helpers in 82BFE2xx-82BFE4xx.
  // Recorder (worker0:1): every wait for a closure marshalled to job worker 0 as a "worker0" event;
  // with load:3 also the "[loading-pacing] worker0 op" line (its 60 s window is gone).
  if(fable2::fr::On(fable2::fr::kWorker0) && lr>=0x82BFE000u && lr<0x82BFF600u) {
    const auto start=std::chrono::steady_clock::now();
    const bool loader=fable2::loading::InBootLoader(ctx,base);
    const bool log=fable2::loading::Trace() && fable2::fr::LogWindowOpen();
    const std::string stack=log ? fable2::skipintro::guest_stack(ctx,base) : std::string();
    uint32_t st[5];
    const int n=fable2::loading::StackOf(ctx,base,st,5);
    const uint64_t m0=mach_absolute_time();
    __imp__ProcessAndWaitMapStatus_82B49B50(ctx,base);
    fable2::fr::EmitStack(fable2::fr::kTWorker0,lr,(loader?1u:0u)|(fable2::loading::OnGameThread()?2u:0u),
        mach_absolute_time()-m0,0,st,n);
    if(log)
      REXSYS_INFO("[loading-pacing] worker0 op actual={:.2f}ms bootloader={} game={} stack {}",
          std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),loader?1:0,
          fable2::loading::OnGameThread()?1:0,stack);
    return;
  }
  __imp__ProcessAndWaitMapStatus_82B49B50(ctx,base);
}
extern "C" void __imp__ProcessAndProcessAndProcess1552_82BFD7F0(PPCContext&,uint8_t*);
extern "C" void __imp__CheckTimeoutCondition_82C08010(PPCContext&,uint8_t*);
extern "C" void CheckTimeoutCondition_82C08010(PPCContext& ctx,uint8_t* base) {
  if(ctx.r3.u32==0x83334BFCu && fable2::loading::DrainPending().load(std::memory_order_relaxed) &&
     fable2::loading::DrainPending().exchange(false,std::memory_order_acq_rel)) {
    const PPCContext saved=ctx;
    __imp__ProcessAndProcessAndProcess1552_82BFD7F0(ctx,base);
    ctx=saved;
    if(fable2::loading::Trace()) {
      static std::atomic<int> drains{0};
      REXSYS_INFO("[loading-pacing] boot loader drain #{} on worker 0",drains.fetch_add(1)+1);
    }
  }
  __imp__CheckTimeoutCondition_82C08010(ctx,base);
}

// Fast title (default on with skip intro; FABLE2_TITLE_FAST=0 restores the original title).
// The "Press A" title screen (vtable 820CD110, object +20 = its Gameface movie, +24 state,
// +72 prompt interactive) is driven by events the movie's timeline fires
// (handlers registered in sub_826C5ED0):
//   INTRO_FINISHED -> sub_826C60C0: prompt interactive (+72 = 1), record time, start the
//                     front-end audio bank preload (sub_82354B58), front-end notify.
//   OUTRO_FINISHED -> sub_826C6150: state 10 (accepted) -> 11; the title update
//                     (ProcessAndProcessAndProcess1723_826C63D8) then opens the main menu.
// With the logo movies skipped, the intro (white, then fade to the title) takes ~2.76 s
// of guest time after the screen appears, and the outro after A takes ~2.0 s. For the
// first title screen of the process only, the update hook calls the INTRO_FINISHED
// handler on the first frame of state 9 and the OUTRO_FINISHED handler on the first
// frame of state 10. All game work behind those events still runs, only sooner; the
// movie's own later INTRO_FINISHED for that screen is dropped once if it arrives before
// A (its OUTRO_FINISHED is already a no-op outside state 10). Returning to the title later is untouched.
namespace fable2::loading {
inline bool TitleFast() {
  static const bool on=[] {
    const char* v=std::getenv("FABLE2_TITLE_FAST");
    return fable2::skipintro::enabled() && !(v && *v=='0');
  }();
  return on;
}
struct TitleFastState {uint32_t title=0;bool intro=false,drop_intro=false,outro=false;};
inline TitleFastState& TitleFastSt() {static TitleFastState s;return s;}
}
extern "C" void __imp__sub_826C60C0(PPCContext&,uint8_t*);
extern "C" void __imp__sub_826C6150(PPCContext&,uint8_t*);
extern "C" void __imp__ProcessAndProcessAndProcess1723_826C63D8(PPCContext&,uint8_t*);
extern "C" void ProcessAndProcessAndProcess1723_826C63D8(PPCContext& ctx,uint8_t* base) {
  auto& st=fable2::loading::TitleFastSt();
  static const bool announced=[] {
    REXSYS_INFO("[title-fast] {}",fable2::loading::TitleFast() ? "on: title intro/outro events sent on the first title screen (FABLE2_TITLE_FAST=0 disables)"
        : fable2::skipintro::enabled() ? "off (FABLE2_TITLE_FAST=0)" : "off (skip intro not enabled)");
    return true;
  }();
  (void)announced;
  if(fable2::loading::TitleFast() && !st.outro) {
    const uint32_t title=ctx.r3.u32;
    if(st.title==0 && REX_LOAD_U32(title)==0x820CD110u)st.title=title;
    if(title==st.title) {
      const uint32_t state=REX_LOAD_U32(title+24);
      const char* fired=nullptr;
      if(!st.intro && state==9 && REX_LOAD_U8(title+72)==0) {
        st.intro=st.drop_intro=true;
        const PPCContext saved=ctx;
        __imp__sub_826C60C0(ctx,base);
        ctx=saved;
        fired="INTRO_FINISHED";
      } else if(st.intro && state==10) {
        st.outro=true;
        st.drop_intro=false;  // a later title screen may reuse this address: never drop its events
        const PPCContext saved=ctx;
        __imp__sub_826C6150(ctx,base);
        ctx=saved;
        fired="OUTRO_FINISHED";
      }
      if(fired)REXSYS_INFO("[title-fast] {} sent to title {:08X} (state {} -> {})",fired,title,state,REX_LOAD_U32(title+24));
    }
  }
  __imp__ProcessAndProcessAndProcess1723_826C63D8(ctx,base);
}

// INTRO_FINISHED handler: drops the movie's own late event after the fast-title one;
// with FABLE2_LOADING_TRACE=1 logs callers.
extern "C" void sub_826C60C0(PPCContext& ctx,uint8_t* base) {
  auto& st=fable2::loading::TitleFastSt();
  if(st.drop_intro && ctx.r3.u32==st.title) {
    st.drop_intro=false;
    REXSYS_INFO("[title-fast] movie INTRO_FINISHED for title {:08X} dropped (already sent; state {})",
        ctx.r3.u32,REX_LOAD_U32(ctx.r3.u32+24));
    return;
  }
  if(fable2::loading::Trace())
    REXSYS_INFO("[loading-pacing] title prompt enable self={:#x} stack {}",ctx.r3.u32,fable2::skipintro::guest_stack(ctx,base));
  __imp__sub_826C60C0(ctx,base);
}

// Level-load call sub_825F3050 (reached from sub_8233EEE0 inside the GameThread frame update; save
// loads and area transitions). Always: the recorder's level_load / loading tags and events (the
// frame-gap trigger ignores gaps inside this call) and the region tag. With load:3 the
// "[loading-pacing] level-load" lines (the 50-call cap is gone).
extern "C" void __imp__sub_825F3050(PPCContext&,uint8_t*);
extern "C" void sub_825F3050(PPCContext& ctx,uint8_t* base) {
  FR_HEARTBEAT("level_load_825F3050");
  // Input recording/replay anchor (src/diagnostics/fable2_session_journal.h): the end of each level load.
  if(!fable2::fr::Active()){__imp__sub_825F3050(ctx,base);fable2::session::Get().Marker("LevelLoaded");return;}
  static std::atomic<int> calls{0};
  const int n=calls.fetch_add(1)+1;
  const auto start=std::chrono::steady_clock::now();
  const uint64_t m0=mach_absolute_time();
  // The region tag comes from a scan of this thread's stack, done by the recorder thread while the
  // call runs (fable2::fr::ScanLevelNames); nothing extra runs on the game thread.
  fable2::fr::LevelLoadBegin(ctx.r3.u32,&ctx,ctx.r1.u32);
  const bool log=fable2::loading::Trace();
  if(log)REXSYS_INFO("[loading-pacing] level-load sub_825F3050 enter #{} r3={:#x}",n,ctx.r3.u32);
  __imp__sub_825F3050(ctx,base);
  fable2::fr::LevelLoadEnd(mach_absolute_time()-m0);
  fable2::session::Get().Marker("LevelLoaded");
  if(log)REXSYS_INFO("[loading-pacing] level-load sub_825F3050 exit #{} {:.1f}ms",n,
      std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
}

// Trace-only (FABLE2_LOADING_TRACE=1): the generic "Sleep(0) until obj->vfunc+12() != 0" wait
// (ProcessAndQueryWithByteCheck_82C6F848, r3 = obj). Logs the object, its vtable and the polled
// function, how long it waited and the caller chain, for waits >= 50 ms. While a wait is past
// 300 ms, NtSetEvent/KeSetEvent calls from any thread are logged with stacks (who ends the stall).
namespace fable2::loading {
inline std::atomic<int64_t>& PollSinceUs() {static std::atomic<int64_t> v{0};return v;}
inline int64_t SteadyUs() {return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
// Ring of set-event calls made while a poll-wait is in progress; dumped (last 150 ms) when a
// poll-wait of >= 300 ms ends.
struct SetEvRec {int64_t us=0;uint32_t r3=0;char thread[24]={};char stack[200]={};};
inline std::mutex& SetEvMu() {static std::mutex m;return m;}
inline std::vector<SetEvRec>& SetEvRing() {static std::vector<SetEvRec> v(256);return v;}
inline size_t& SetEvPos() {static size_t p=0;return p;}
inline void TraceSetEvent(const char* who,PPCContext& ctx,uint8_t* base) {
  const int64_t since=PollSinceUs().load(std::memory_order_relaxed);
  if(!since)return;
  // Recorder (signal:1): set-event / release-semaphore calls while a poll-wait has been open > 300 ms.
  if(fable2::fr::On(fable2::fr::kSignal) && SteadyUs()-since>300000) {
    uint32_t st[7];
    const int n=StackOf(ctx,base,st,7);
    fable2::fr::EmitStack(fable2::fr::kTSetEvent,ctx.r3.u32,0,0,0,st,n);
  }
  if(!Trace())return;
  SetEvRec r;r.us=SteadyUs();r.r3=ctx.r3.u32;
  pthread_getname_np(pthread_self(),r.thread,sizeof r.thread);
  std::snprintf(r.stack,sizeof r.stack,"%s %s",who,fable2::skipintro::guest_stack(ctx,base).c_str());
  std::lock_guard<std::mutex> lock(SetEvMu());
  SetEvRing()[SetEvPos()++%SetEvRing().size()]=r;
}
inline void DumpSetEvents(int64_t end_us) {
  std::lock_guard<std::mutex> lock(SetEvMu());
  auto& ring=SetEvRing();
  for(size_t i=0;i<ring.size();++i) {
    const SetEvRec& r=ring[(SetEvPos()+i)%ring.size()];
    if(r.us && end_us-r.us<=150000)
      REXSYS_INFO("[loading-pacing] poll-wait end-150ms: {:+.1f}ms r3={:#x} '{}' {}",(r.us-end_us)/1000.0,r.r3,r.thread,r.stack);
  }
  for(auto& r:ring)r.us=0;
}
}
extern "C" void __imp__ProcessAndQueryWithByteCheck_82C6F848(PPCContext&,uint8_t*);
extern "C" void ProcessAndQueryWithByteCheck_82C6F848(PPCContext& ctx,uint8_t* base) {
  FR_HEARTBEAT("poll_wait_82C6F848");
  if(!fable2::fr::On(fable2::fr::kWait)){__imp__ProcessAndQueryWithByteCheck_82C6F848(ctx,base);return;}
  const uint32_t obj=ctx.r3.u32,vt=REX_LOAD_U32(obj),poll=REX_LOAD_U32(vt+12);
  const bool trace=fable2::loading::Trace();
  const std::string stack=trace ? fable2::skipintro::guest_stack(ctx,base) : std::string();
  const uint32_t lr0=uint32_t(ctx.lr);
  const uint64_t m0=mach_absolute_time();
  const int64_t t0=fable2::loading::SteadyUs();
  int64_t expected=0;
  const bool owner=fable2::loading::PollSinceUs().compare_exchange_strong(expected,t0);
  __imp__ProcessAndQueryWithByteCheck_82C6F848(ctx,base);
  const int64_t t1=fable2::loading::SteadyUs();
  if(owner)fable2::loading::PollSinceUs().store(0);
  const double ms=(t1-t0)/1000.0;
  if(ms>=50.0) {
    uint32_t st[15];
    const int sn=fable2::loading::StackOf(ctx,base,st,15,lr0);  // r1 is restored after the call
    fable2::fr::EmitStack(fable2::fr::kTPollWait,obj,poll,mach_absolute_time()-m0,0,st,sn);
  }
  if(!trace)return;
  if(owner && ms>=300.0)fable2::loading::DumpSetEvents(t1);
  if(ms>=50.0)
    REXSYS_INFO("[loading-pacing] poll-wait obj={:#x} vtable={:#x} poll={:#x} vfunc16={:#x} actual={:.1f}ms stack {}",
        obj,vt,poll,REX_LOAD_U32(vt+16),ms,stack);
}
#define FABLE2_SETEVENT_TRACE(sym) \
  extern "C" void __imp__##sym(PPCContext& ctx,uint8_t* base) { \
    static const auto real=reinterpret_cast<fable2::loading::prof::Import>(dlsym(RTLD_NEXT,"__imp__" #sym)); \
    FR_HEARTBEAT(#sym); \
    fable2::loading::TraceSetEvent(#sym,ctx,base); \
    real(ctx,base); \
  }
FABLE2_SETEVENT_TRACE(NtSetEvent)
FABLE2_SETEVENT_TRACE(KeSetEvent)
FABLE2_SETEVENT_TRACE(NtReleaseSemaphore)
FABLE2_SETEVENT_TRACE(KeReleaseSemaphore)
#undef FABLE2_SETEVENT_TRACE
// Trace-only: the file-request dispatcher's per-device pass (r3 = device; +20..+24 = vector of
// in-flight requests, +12 = pending count). While a poll-wait is past 300 ms, logs each in-flight
// request's vtable and its done/issue functions, once per second.
extern "C" void __imp__ProcessAndProcessAndProcess1098_82B44400(PPCContext&,uint8_t*);
extern "C" void ProcessAndProcessAndProcess1098_82B44400(PPCContext& ctx,uint8_t* base) {
  if(fable2::loading::Trace()) {
    const int64_t since=fable2::loading::PollSinceUs().load(std::memory_order_relaxed);
    static std::atomic<int64_t> last{0};
    const int64_t now=fable2::loading::SteadyUs();
    if(since && now-since>300000 && (now-last.load()>900000 || now-last.load()<3000)) {
      if(now-last.load()>900000)last.store(now);
      const uint32_t dev=ctx.r3.u32,b=REX_LOAD_U32(dev+20),e=REX_LOAD_U32(dev+24);
      std::string line;
      for(uint32_t p=b;p && p<e && p-b<64;p+=4) {
        const uint32_t req=REX_LOAD_U32(p),vt=REX_LOAD_U32(req);
        char buf[160];
        std::snprintf(buf,sizeof buf," [req %08X vt %08X done %08X issue %08X w:",req,vt,REX_LOAD_U32(vt+12),REX_LOAD_U32(vt+24));
        line+=buf;
        for(int k=0;k<16;++k){std::snprintf(buf,sizeof buf," %08X",REX_LOAD_U32(req+4*k));line+=buf;}
        line+="]";
      }
      REXSYS_INFO("[loading-pacing] dispatcher dev={:#x} pending={} max={} inflight:{}",dev,REX_LOAD_U32(dev+12),REX_LOAD_U32(dev+32),line);
    }
  }
  __imp__ProcessAndProcessAndProcess1098_82B44400(ctx,base);
}
// Trace-only: a finished request posted to the completion thread (r3 = completion object,
// r4 = request) while a poll-wait is past 300 ms.
extern "C" void __imp__ProcessAndProcessAndProcess1711_82B454C0(PPCContext&,uint8_t*);
extern "C" void ProcessAndProcessAndProcess1711_82B454C0(PPCContext& ctx,uint8_t* base) {
  if(fable2::loading::Trace()) {
    const int64_t since=fable2::loading::PollSinceUs().load(std::memory_order_relaxed);
    if(since && fable2::loading::SteadyUs()-since>300000) {
      const uint32_t req=ctx.r4.u32,vt=REX_LOAD_U32(req);
      std::string w;char buf[16];
      for(int k=0;k<20;++k){std::snprintf(buf,sizeof buf," %08X",REX_LOAD_U32(req+4*k));w+=buf;}
      REXSYS_INFO("[loading-pacing] stall completion req={:#x} vt={:#x} done={:#x} issue={:#x} complete={:#x} w:{} stack {}",req,vt,
          REX_LOAD_U32(vt+12),REX_LOAD_U32(vt+24),REX_LOAD_U32(vt+28),w,fable2::skipintro::guest_stack(ctx,base));
    }
  }
  __imp__ProcessAndProcessAndProcess1711_82B454C0(ctx,base);
}
// Trace-only: any insert into a lock-free request/completion queue (r3 = queue, r4 = item)
// while a poll-wait is past 300 ms (first 60 per session).
extern "C" void __imp__ConstructAndInsertNode_82B456C8(PPCContext&,uint8_t*);
extern "C" void ConstructAndInsertNode_82B456C8(PPCContext& ctx,uint8_t* base) {
  if(fable2::loading::Trace()) {
    const int64_t since=fable2::loading::PollSinceUs().load(std::memory_order_relaxed);
    static std::atomic<int> n{0};
    if(since && fable2::loading::SteadyUs()-since>300000 && n.fetch_add(1)<60) {
      const uint32_t item=ctx.r4.u32,vt=REX_LOAD_U32(item);
      char name[64]="";pthread_getname_np(pthread_self(),name,sizeof name);
      REXSYS_INFO("[loading-pacing] stall enqueue queue={:#x} item={:#x} vt={:#x} vf12={:#x} vf24={:#x} vf28={:#x} thread '{}' stack {}",
          ctx.r3.u32,item,vt,REX_LOAD_U32(vt+12),REX_LOAD_U32(vt+24),REX_LOAD_U32(vt+28),name,fable2::skipintro::guest_stack(ctx,base));
    }
  }
  __imp__ConstructAndInsertNode_82B456C8(ctx,base);
}

// Streaming read budget (default on; FABLE2_STREAM_BUDGET=0 restores the original limiter).
// Requests of the file-request dispatcher (thread at 82B44D10, 500 ms alertable waits) are
// issued through ProcessAndProcessAndProcess2114_82B4ED00, which asks a global limiter
// (singleton 834C3510, getter ProcessAndAllocateAndProcess11_82B4DF08, built in sub_82B4DF88)
// before every issue and returns -3 ("busy, keep it pending") when either
//   +72 bytes issued in the sliding window (+56, a double in seconds) > +64 budget (3 MB), or
//   +76 bytes in flight > +68 cap (15 MB).
// The window entries are aged by guest timebase (ProcessAndTimebaseAndProcess2_82B4E508). It is a
// DVD-era bandwidth cap: on the console a level load's reads spread over seconds, so the window
// drained as it went. Here the level load reads 63 MB in ~13 ms, the window is full, and every
// streaming request behind it (23 pending at the area transition) waits until the old entries age
// out: 5 s, or 10 s when a second burst follows. The 3D Engine meanwhile polls a stream object
// it stopped (sub_82AB7CD0 -> 82C6F848 "Sleep(0) until +48 != 0") and no frame is presented.
// This raises only the windowed budget (+64) to 0x7FFFFFFF; the in-flight memory cap (+68),
// the window bookkeeping and every request are unchanged.
namespace fable2::loading {
inline bool StreamBudgetFix() {
  static const bool on=[] {const char* v=std::getenv("FABLE2_STREAM_BUDGET");return !(v && *v=='0');}();
  return on;
}
}
extern "C" void __imp__ProcessAndAllocateAndProcess11_82B4DF08(PPCContext&,uint8_t*);
extern "C" void ProcessAndAllocateAndProcess11_82B4DF08(PPCContext& ctx,uint8_t* base) {
  __imp__ProcessAndAllocateAndProcess11_82B4DF08(ctx,base);
  const uint32_t lim=ctx.r3.u32;
  static std::atomic<bool> announced{false};
  if(!fable2::loading::StreamBudgetFix()) {
    if(!announced.exchange(true))REXSYS_INFO("[stream-budget] off (FABLE2_STREAM_BUDGET=0): original 5 s streaming bandwidth window");
    return;
  }
  if(REX_LOAD_U32(lim+64)!=0x7FFFFFFFu) {
    if(!announced.exchange(true)) {
      double window=0;uint64_t bits=REX_LOAD_U64(lim+56);std::memcpy(&window,&bits,sizeof window);
      REXSYS_INFO("[stream-budget] on: streaming window budget {} bytes per {:.2f} s raised to unlimited "
          "(in-flight cap {} kept; FABLE2_STREAM_BUDGET=0 disables)",REX_LOAD_U32(lim+64),window,REX_LOAD_U32(lim+68));
    }
    REX_STORE_U32(lim+64,0x7FFFFFFFu);
  }
}

// Save pacing (both default on).
// 1) "Saving content" minimum display: after the save is written and closed, sub_824EC3B0 sleeps
//    max(0, K - elapsed) seconds (K = 3.1 (logged as "game 3100 ms"), the double at 8209E728; * 1000 at 82000DC8) through
//    DelayZero_82CBC6B0 (return 824EC440), so the notice stays up >= 3 s (a TCR rule; the 2.4 s
//    is pure padding). FABLE2_SAVE_NOTICE_MS=<ms> sets the minimum (default 500, 0 with FABLE2_FAST_PACING=1;
//    =3100 restores).
// 2) sub_824EC168 shows the notice, then Sleep(100) (return 824EC274) before mounting save:\.
//    Pure pacing; skipped by default (FABLE2_SAVE_FAST_MOUNT=0 restores it).
namespace fable2::loading {
inline int SaveNoticeMs() {
  static const int ms=[] {
    // FABLE2_FAST_PACING=1 ("Skip cosmetic waits", fable2_fast_pacing.h) makes 0 the default; an explicit value wins.
    const int def=fable2::pacing::FastPacing() ? 0 : 500;
    const char* v=std::getenv("FABLE2_SAVE_NOTICE_MS");
    if(!v || !*v)return def;
    char* end=nullptr;const long p=std::strtol(v,&end,10);
    return end && !*end && p>=0 && p<=10000 ? int(p) : def;
  }();
  return ms;
}
inline bool SaveFastMount() {
  static const bool on=[] {const char* v=std::getenv("FABLE2_SAVE_FAST_MOUNT");return !(v && *v=='0');}();
  return on;
}
inline double GuestDouble(uint8_t* base,uint32_t addr) {
  const uint64_t bits=REX_LOAD_U64(addr);double d;std::memcpy(&d,&bits,sizeof d);return d;
}
}
extern "C" void __imp__DelayZero_82CBC6B0(PPCContext&,uint8_t*);
extern "C" void DelayZero_82CBC6B0(PPCContext& ctx,uint8_t* base) {
  const uint32_t lr=uint32_t(ctx.lr);
  if(lr==0x824EC440) {
    FR_HEARTBEAT("save_notice_824EC440");
    const double k=fable2::loading::GuestDouble(base,0x8209E728u),unit=fable2::loading::GuestDouble(base,0x82000DC8u);
    const int64_t full=int64_t(k*unit+0.5),requested=ctx.r3.s32,target=fable2::loading::SaveNoticeMs();
    int64_t ms=requested;
    if(target<full && full>0 && requested>=0)ms=std::max<int64_t>(0,requested-(full-target));
    REXSYS_INFO("[save-notice] minimum {} ms (game {} ms; FABLE2_SAVE_NOTICE_MS): elapsed {} ms, sleep {} ms instead of {} ms",
        target<full ? target : full,full,full-requested,ms,requested);
    if(fable2::fr::On(fable2::fr::kSave))fable2::fr::Emit(fable2::fr::kTSaveNotice,uint32_t(requested),uint32_t(ms),0);
    fable2::fr::SetTag(fable2::fr::kTagSaveNotice,fable2::fr::GetTag(fable2::fr::kTagSaveNotice)+1);
    ctx.r3.s64=ms;
  } else if(lr==0x824EC274 && ctx.r3.u32==100 && fable2::loading::SaveFastMount()) {
    static std::atomic<bool> once{false};
    if(!once.exchange(true))REXSYS_INFO("[save-notice] Sleep(100) before the save mount skipped (FABLE2_SAVE_FAST_MOUNT=0 restores)");
    ctx.r3.s64=0;
  }
  __imp__DelayZero_82CBC6B0(ctx,base);
}
// XamContentClose timing (the save is fully written when it returns): recorder event (save:1);
// with load:3 also a log line.
extern "C" void __imp__XamContentClose(PPCContext& ctx,uint8_t* base) {
  static const auto real=reinterpret_cast<fable2::loading::prof::Import>(dlsym(RTLD_NEXT,"__imp__XamContentClose"));
  FR_HEARTBEAT("XamContentClose");
  const uint64_t m0=mach_absolute_time();
  if(!fable2::loading::Trace()) {
    real(ctx,base);
    if(fable2::fr::On(fable2::fr::kSave))fable2::fr::Emit(fable2::fr::kTContentClose,ctx.r3.u32,0,mach_absolute_time()-m0);
    return;
  }
  const uint64_t t0=fable2::loading::prof::NowNs();
  const std::string stack=fable2::skipintro::guest_stack(ctx,base);
  real(ctx,base);
  if(fable2::fr::On(fable2::fr::kSave))fable2::fr::Emit(fable2::fr::kTContentClose,ctx.r3.u32,0,mach_absolute_time()-m0);
  REXSYS_INFO("[loading-pacing] XamContentClose result={:#x} {:.2f}ms stack {}",ctx.r3.u32,(fable2::loading::prof::NowNs()-t0)/1e6,stack);
}
