// Regression for safe handling of protected guest pages on 16-KiB macOS hosts.
#include <rex/ppc/context.h>
#include "fable2_checkpoint.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
int main() {
  const size_t page=size_t(sysconf(_SC_PAGESIZE));
  auto* memory=static_cast<uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0));
  assert(memory!=MAP_FAILED);
  std::vector<uint8_t> captured(page);
  memory[0]=42;
  assert(fable2::checkpoint::ReadChecked(memory,captured.data(),page));
  assert(captured[0]==42);
  assert(fable2::checkpoint::Writable(memory,page*2));
  assert(mprotect(memory+page,page,PROT_NONE)==0);
  // A snapshot must return an error, without entering the guest fault handler.
  assert(!fable2::checkpoint::ReadChecked(memory+page,captured.data(),page));
  assert(!fable2::checkpoint::Writable(memory,page*2));
  assert(fable2::checkpoint::Writable(memory,page));
  assert(mprotect(memory,page,PROT_READ)==0);
  assert(fable2::checkpoint::ReadChecked(memory,captured.data(),page));
  assert(!fable2::checkpoint::Writable(memory,page));
  assert(mprotect(memory,page*2,PROT_READ|PROT_WRITE)==0);
  memory[0]=99;
  assert(mach_vm_write(mach_task_self(),reinterpret_cast<mach_vm_address_t>(memory),
      reinterpret_cast<vm_offset_t>(captured.data()),page)==KERN_SUCCESS);
  assert(memory[0]==42);
  assert(munmap(memory,page*2)==0);
}
