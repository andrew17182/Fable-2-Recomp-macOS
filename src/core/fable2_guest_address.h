#pragma once
// Bounds- and protection-checked host pointer for a guest address (read-only
// diagnostics; returns nullptr instead of faulting on an unmapped page).
#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>
namespace fable2::guestmem {
inline uint8_t* CheckedGuestAddress(uint8_t* base,uint32_t at,uint32_t size,bool write) {
  if(uint64_t(at)+size>0x100000000ull)return nullptr;
  // Matches generated/default/fable_2_pch.h REX_PHYS_HOST_OFFSET and the
  // runtime physical heap's host_address_offset on this macOS ARM64 port.
  uint64_t offset=at;
#if defined(__aarch64__) || defined(__arm64__)
  if(at>=0xE0000000u)offset+=0x1000u;
#endif
  mach_vm_address_t address=reinterpret_cast<mach_vm_address_t>(base+offset);
  mach_vm_address_t region=address;mach_vm_size_t length=0;
  vm_region_basic_info_data_64_t info{};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object=MACH_PORT_NULL;
  auto result=mach_vm_region(mach_task_self(),&region,&length,VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info),&count,&object);
  if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
  auto required=write?(VM_PROT_READ|VM_PROT_WRITE):VM_PROT_READ;
  if(result!=KERN_SUCCESS || region>address || address+size>region+length ||
      (info.protection&required)!=required)return nullptr;
  return base+offset;
}
}
