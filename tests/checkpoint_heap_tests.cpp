// Exercise allocator metadata and physical alias reconstruction using the runtime.
#include <rex/ppc/context.h>
#include "fable2_checkpoint.h"
#include <cassert>
int main() {
  rex::InitLogging(nullptr,spdlog::level::warn);
  rex::memory::Memory memory;
  assert(memory.Initialize());
  auto* alias=static_cast<rex::memory::PhysicalHeap*>(memory.LookupHeap(0xE0000000));
  auto* parent=memory.GetPhysicalHeap();
  const uint32_t address=0xE1000000, size=3*4096;
  const uint32_t physical=alias->GetPhysicalAddress(address);
  const uint32_t flags=rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit;
  const uint32_t protect=rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite;
  assert(fable2::checkpoint::EntirelyFree(alias,address,size));
  assert(fable2::checkpoint::EntirelyFree(parent,physical,size));
  auto alias_free=alias->unreserved_page_count(), parent_free=parent->unreserved_page_count();
  assert(alias->AllocFixed(address,size,4096,flags,protect));
  auto* bytes=memory.TranslatePhysical(physical);
  bytes[0]=42; bytes[size-1]=73;
  std::vector<uint8_t> captured(size);
  assert(fable2::checkpoint::ReadChecked(bytes,captured.data(),size));
  assert(alias->Release(address));
  assert(alias->unreserved_page_count()==alias_free);
  assert(parent->unreserved_page_count()==parent_free);
  {
    fable2::checkpoint::Recreated recreated;
    recreated.Create({alias,address,size,protect});
    rex::memory::HeapAllocationInfo a{},p{};
    assert(alias->QueryRegionInfo(address,&a));
    assert(parent->QueryRegionInfo(physical,&p));
    assert(a.allocation_base==address && p.allocation_base==physical);
    assert(a.allocation_size==size && p.allocation_size==size);
    assert(a.state==flags && p.state==flags);
    assert(fable2::checkpoint::Writable(bytes,size));
    assert(mach_vm_write(mach_task_self(),reinterpret_cast<mach_vm_address_t>(bytes),
        reinterpret_cast<vm_offset_t>(captured.data()),size)==KERN_SUCCESS);
    assert(bytes[0]==42 && bytes[size-1]==73);
    // Simulated post-allocation preflight failure rolls both ledgers back.
  }
  assert(fable2::checkpoint::EntirelyFree(alias,address,size));
  assert(fable2::checkpoint::EntirelyFree(parent,physical,size));
  assert(alias->unreserved_page_count()==alias_free);
  assert(parent->unreserved_page_count()==parent_free);
}
