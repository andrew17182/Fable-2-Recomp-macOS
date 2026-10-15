#pragma once
// Observation only: the last successful acquire is not a serialized owner or
// a release ledger. Inline stores still release the original guest lock.
#include "core/fable2_guest_address.h"
#include <rex/system/xthread.h>
#include <pthread.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
namespace fable2::guest_lock_trace {
struct Acquisition {uint32_t guest_tid=0,caller=0;uint64_t os_tid=0,time_ns=0;bool converted=false;};
inline std::mutex mutex;
inline Acquisition last;
inline uint32_t Watched() {
  static const uint32_t address=[](){const auto value=std::getenv("FABLE2_WATCH_GUEST_LOCK");return value?uint32_t(std::strtoul(value,nullptr,0)):0;}();
  return address;
}
inline uint32_t ThreadId() {auto thread=rex::system::XThread::GetCurrentThread();return thread?thread->thread_id():0;}
inline void Acquired(uint32_t address,uint32_t caller,bool converted) {
  if(!Watched() || address!=Watched())return;
  Acquisition value;value.guest_tid=ThreadId();value.caller=caller;value.converted=converted;
  pthread_threadid_np(nullptr,&value.os_tid);
  value.time_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  std::lock_guard lock(mutex);last=value;
  static unsigned written=0;const auto dir=std::getenv("FABLE2_TEST_CONTROL_DIR");
  if(dir && written++<1024) {
    std::ofstream file(std::string(dir)+"/guest-lock-acquisitions.log",std::ios::app);
    file<<"acquired address=0x"<<std::hex<<address<<" caller=0x"<<caller<<std::dec
        <<" guest_tid="<<value.guest_tid<<" os_tid="<<value.os_tid
        <<" converted="<<converted<<" time_ns="<<value.time_ns<<'\n';
  }
}
inline std::string Describe(uint8_t* base,uint32_t address) {
  if(!Watched() || address!=Watched())return {};
  Acquisition value;{std::lock_guard lock(mutex);value=last;}
  std::ostringstream result;result<<" watched_lock=0x"<<std::hex<<address;
  if(auto pointer=guestmem::CheckedGuestAddress(base,address,4,false)) {
    uint32_t word;std::memcpy(&word,pointer,4);result<<" word=0x"<<__builtin_bswap32(word);
  }
  result<<std::dec<<" last_acquirer_guest_tid="<<value.guest_tid<<" last_acquirer_os_tid="<<value.os_tid
      <<" last_acquirer_converted="<<value.converted<<" acquire_caller=0x"<<std::hex<<value.caller
      <<std::dec<<" acquired_ns="<<value.time_ns;
  return result.str();
}
}
extern "C" void SpinLock_Acquire_822D7408(PPCContext& ctx,uint8_t* base) {
  const auto address=ctx.r3.u32,caller=uint32_t(ctx.lr);
  __imp__SpinLock_Acquire_822D7408(ctx,base);
  fable2::guest_lock_trace::Acquired(address,caller,false);
}
