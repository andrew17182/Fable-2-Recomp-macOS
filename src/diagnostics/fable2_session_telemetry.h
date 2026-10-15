#pragma once
#include "fable2_session_journal.h"
#include "fable2_benchmark_pad.h"
#include <array>
#include <algorithm>
#include <vector>
#include <rex/ppc/context.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
namespace fable2::session {
// Fingerprint the actual rendered UI structure, using fixed guest vtable
// addresses and item counts, never host/heap addresses or screenshots.
inline bool Read(const uint8_t* base,uint32_t address,void* out,size_t n) {
#ifdef __APPLE__
  if(address<0x1000 || address>=0xE0000000u || n>0xE0000000u-address) return false;
  mach_vm_size_t copied=0;
  return mach_vm_read_overwrite(mach_task_self(),
    reinterpret_cast<mach_vm_address_t>(base+address),n,
    reinterpret_cast<mach_vm_address_t>(out),&copied)==KERN_SUCCESS && copied==n;
#else
  (void)base;(void)address;(void)out;(void)n;return false;
#endif
}
inline thread_local std::vector<uint32_t> frame_items;
inline thread_local int frame_depth=0;
struct FrameScope {
  bool active;
  FrameScope():active(Get().Enabled()) {
    if(active && frame_depth++==0) frame_items.clear();
  }
  ~FrameScope() {
    if(!active || --frame_depth!=0) return;
    std::sort(frame_items.begin(),frame_items.end());
    uint64_t hash=14695981039346656037ull;
    for(auto item:frame_items) {hash^=item;hash*=1099511628211ull;}
    std::ostringstream name;
    name<<"UI:"<<frame_items.size()<<":"<<std::hex<<hash;
    Get().Signature(name.str());
  }
};
inline void ObserveText(const uint8_t* base,uint32_t item) {
  if(!frame_depth || frame_items.size()>=4096) return;
  std::array<uint8_t,4> bytes{};
  if(!Read(base,item,bytes.data(),bytes.size())) return;
  uint32_t vt=uint32_t(bytes[0])<<24|uint32_t(bytes[1])<<16|uint32_t(bytes[2])<<8|bytes[3];
  frame_items.push_back(vt>=0x82000000 && vt<0x84000000 ? vt : 0);
  // Dispatch containers hold widget pointers rather than their own vtable.
  std::array<uint8_t,24> children{};
  if(!Read(base,item,children.data(),children.size())) return;
  for(size_t i=0;i<children.size();i+=4) {
    uint32_t ptr=uint32_t(children[i])<<24|uint32_t(children[i+1])<<16|uint32_t(children[i+2])<<8|children[i+3];
    if(!Read(base,ptr,bytes.data(),bytes.size())) continue;
    uint32_t type=uint32_t(bytes[0])<<24|uint32_t(bytes[1])<<16|uint32_t(bytes[2])<<8|bytes[3];
    if(type>=0x82000000 && type<0x84000000) frame_items.push_back(type);
  }
}
inline void ObservePad(PPCContext& ctx,uint8_t* base,uint32_t pointer,uint32_t user) {
  // Built-in benchmark (src/core/fable2_benchmark.h): replaces the pad like a replay would.
  const bool bench=fable2::bench::g_pad.active.load(std::memory_order_relaxed);
  auto& j=Get();if((!bench&&!j.Enabled())||ctx.r3.u32!=0||user!=0) return;
  auto* p=base+pointer;
  auto u16=[](const uint8_t* b){return (unsigned(b[0])<<8)|b[1];};
  Pad pad{int(u16(p+4)),p[6],p[7],int16_t(u16(p+8)),int16_t(u16(p+10)),
          int16_t(u16(p+12)),int16_t(u16(p+14))};
  if(bench) {
    auto& o=fable2::bench::g_pad;
    pad=Pad{o.buttons.load(std::memory_order_relaxed),0,0,int16_t(o.lx.load(std::memory_order_relaxed)),
            int16_t(o.ly.load(std::memory_order_relaxed)),
            o.rx.load(std::memory_order_relaxed),o.ry.load(std::memory_order_relaxed)};
  } else if(!j.Input(pad)) return;
  auto put=[](uint8_t* b,unsigned v){b[0]=v>>8;b[1]=v;};
  static std::mutex packet_mutex;std::lock_guard lock(packet_mutex);
  static Pad previous;static unsigned packet=0;
  if(pad!=previous) {++packet;previous=pad;}
  p[0]=packet>>24;p[1]=packet>>16;p[2]=packet>>8;p[3]=packet;
  put(p+4,pad.buttons);p[6]=pad.lt;p[7]=pad.rt;
  put(p+8,pad.lx);put(p+10,pad.ly);put(p+12,pad.rx);put(p+14,pad.ry);
}
} // namespace fable2::session
