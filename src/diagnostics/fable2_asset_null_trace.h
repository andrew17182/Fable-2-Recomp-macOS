#pragma once
// Diagnose the observed native texture-streamer's null retain target without
// changing dispatch or lifetime behavior. Enabled only in an owned test run.
#include "core/fable2_guest_address.h"
#include <fstream>
#include <cstdlib>
#include <cstring>
extern "C" void ProcessAndProcessAndProcess1661_82B55DF8(PPCContext& ctx,uint8_t* base) {
  const auto enabled=std::getenv("FABLE2_ASSET_NULL_CALL_TRACE");
  const auto directory=std::getenv("FABLE2_TEST_CONTROL_DIR");
  if(enabled && std::string(enabled)=="1" && directory) {
    const auto begin=ctx.r4.u32,end=ctx.r5.u32;
    auto read=[&](uint32_t at,uint32_t& value) {
      auto pointer=fable2::guestmem::CheckedGuestAddress(base,at,4,false);
      if(!pointer)return false;std::memcpy(&value,pointer,4);value=__builtin_bswap32(value);return true;
    };
    if(end>=begin && (end-begin)%16==0 && end-begin<=65536) {
      for(uint64_t at=begin;at<end;at+=16) {
        uint32_t object=0,table=0,target=0;
        if(!read(uint32_t(at),object) || !object || !read(object,table))continue;
        if(uint64_t(table)+4>UINT32_MAX || !read(table+4,target) || target)continue;
        std::ofstream log(std::string(directory)+"/asset-null-target.log",std::ios::app);
        log<<"null-retain caller=0x"<<std::hex<<uint32_t(ctx.lr)<<" begin=0x"<<begin
           <<" end=0x"<<end<<" destination=0x"<<ctx.r6.u32<<" record=0x"<<at
           <<" object=0x"<<object<<" table=0x"<<table<<" words=";
        for(unsigned i=0;i<16;++i) {
          uint32_t value=0;if(uint64_t(object)+i*4>UINT32_MAX || !read(object+i*4,value))break;
          log<<"0x"<<value<<',';
        }
        log<<'\n';
      }
    }
  }
  // Forward the unchanged architectural context to the exact original body.
  __imp__ProcessAndProcessAndProcess1661_82B55DF8(ctx,base);
}
