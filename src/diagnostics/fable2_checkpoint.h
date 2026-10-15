#pragma once
// Experimental same-process gameplay rewind. Host continuations are retained;
// this is deliberately NOT a persistent/full-machine save state.
#ifdef __APPLE__
#ifdef FABLE2_CONTINUATION_NATIVE_TRACE
#include "core/continuation/native_import_trace_api.h"
#endif
#include <rex/runtime.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/register_file.h>
#include <array>
#include <rex/audio/audio_system.h>
#include <rex/stream.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cstring>

namespace fable2::checkpoint {
// Read once: ObserveLock runs on every guest critical-section call, and a per-call
// getenv (environ lock) cost ~1.4 s of save-load time (docs/archive/load-profile-20261006.md).
inline bool Enabled() {
  static const bool enabled = std::getenv("FABLE2_CHECKPOINT_DIR") != nullptr;
  return enabled;
}
inline std::mutex operation_mutex;
inline std::mutex lock_mutex;
inline std::set<uint32_t> locks;
inline void ObserveLock(uint32_t address) {
  if (!Enabled()) return;
  std::lock_guard guard(lock_mutex);
  locks.insert(address);
}
struct Region { rex::memory::BaseHeap* heap; uint32_t address, size, state, protect, allocation_base, allocation_size; };
struct Keep { uint8_t* address; std::vector<uint8_t> bytes; };
inline std::vector<Region> Layout(rex::memory::Memory* memory) {
  std::vector<Region> result;
  std::vector<rex::memory::BaseHeap*> heaps;
  for (uint32_t start : {0u, 0x40000000u, 0x80000000u, 0x90000000u}) heaps.push_back(memory->LookupHeap(start));
  heaps.push_back(memory->GetPhysicalHeap());
  for (auto* heap : heaps) {
    uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
    for (uint64_t at = heap->heap_base(); at < end;) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(at), &info) || !info.region_size)
        throw std::runtime_error("Cannot inspect guest heap layout");
      result.push_back({heap, uint32_t(at), info.region_size, info.state, info.protect, info.allocation_base, info.allocation_size});
      at += info.region_size;
    }
  }
  return result;
}
struct Chunk {
  uint8_t* address; Region region;
  rex::memory::BaseHeap* allocation_heap; uint32_t allocation_address;
  std::vector<uint8_t> bytes;
};
inline bool EntirelyFree(rex::memory::BaseHeap* heap,uint32_t address,uint32_t size) {
  const uint64_t end=uint64_t(address)+size;
  for (uint64_t at=address; at<end;) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(at),&info) || !info.region_size || info.state) return false;
    at+=std::min(uint64_t(info.region_size),end-at);
  }
  return true;
}
struct Allocation {
  rex::memory::BaseHeap* heap; uint32_t address,size,protect;
};
struct Recreated {
  std::vector<Allocation> allocations;
  bool committed=false;
  ~Recreated() {
    if (!committed) for (auto i=allocations.rbegin(); i!=allocations.rend(); ++i) i->heap->Release(i->address);
  }
  void Create(const Allocation& allocation) {
    allocations.push_back(allocation); // Include partial failures in rollback.
    if (!allocation.heap->AllocFixed(allocation.address,allocation.size,allocation.heap->page_size(),
        rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,allocation.protect))
      throw std::runtime_error("Failed to recreate saved allocation");
  }
};
inline std::vector<Chunk> saved_chunks;
inline size_t saved_size = 0;
inline bool ReadChecked(uint8_t* address, uint8_t* output, size_t bytes) {
  mach_vm_size_t copied=0;
  return mach_vm_read_overwrite(mach_task_self(),reinterpret_cast<mach_vm_address_t>(address),
      bytes,reinterpret_cast<mach_vm_address_t>(output),&copied)==KERN_SUCCESS && copied==bytes;
}
inline bool Writable(uint8_t* pointer,size_t length) {
  mach_vm_address_t address=reinterpret_cast<mach_vm_address_t>(pointer);
  const auto end=address+length;
  while (address<end) {
    mach_vm_address_t found=address; mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object=MACH_PORT_NULL;
    auto result=mach_vm_region(mach_task_self(),&found,&size,VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info),&count,&object);
    if (object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
    if (result!=KERN_SUCCESS || found>address || !size || !(info.protection&VM_PROT_WRITE)) return false;
    address=std::min(end,found+size);
  }
  return true;
}
inline uint32_t saved_owner = 0;
inline std::vector<std::pair<uint32_t, uint32_t>> saved_objects;
inline uint8_t saved_gpu[128*1024]{};
inline size_t saved_gpu_size = 0;

inline void Status(const std::filesystem::path& directory, const std::string& message) {
  std::ofstream output(directory / "checkpoint-status.tmp");
  output << message << '\n'; output.close();
  std::filesystem::rename(directory / "checkpoint-status.tmp", directory / "checkpoint-status.txt");
}
// Mach suspension acknowledges completion before returning, unlike the runtime
// signal-based Suspend() call. Counts are balanced even on exceptions.
struct Stopped {
  std::vector<mach_port_t> ports;
  ~Stopped() { for (auto p : ports) thread_resume(p); }
  void Stop(rex::system::XThread* thread) {
    if (!thread->thread() || !thread->is_running()) return;
    auto native = reinterpret_cast<pthread_t>(thread->thread()->native_handle());
    if (pthread_equal(native, pthread_self())) return;
    auto port = pthread_mach_thread_np(native);
    if (!port || thread_suspend(port) != KERN_SUCCESS)
      throw std::runtime_error("Could not suspend guest worker");
    ports.push_back(port);
  }
};
struct Subsystems {
  using GpuFunction = bool (*)(int, rex::system::IGraphicsSystem*, uint8_t*, size_t*);
  GpuFunction gpu_function;
  rex::system::IGraphicsSystem* gpu;
  rex::audio::AudioSystem* audio;
  bool gpu_paused = false, audio_paused = false;
  explicit Subsystems(rex::Runtime* runtime)
      : gpu_function(reinterpret_cast<GpuFunction>(dlsym(RTLD_DEFAULT,"Fable2CheckpointGpu"))),
        gpu(runtime->graphics_system()),
        audio(dynamic_cast<rex::audio::AudioSystem*>(runtime->audio_system())) {
    if (!gpu || !gpu_function) throw std::runtime_error("Unsupported graphics backend");
  }
  void Pause() {
    if (audio) { audio->Pause(); audio_paused = true; }
    if (!gpu_function(0,gpu,nullptr,nullptr)) throw std::runtime_error("GPU checkpoint pause failed");
    gpu_paused = true;
  }
  ~Subsystems() {
    if (gpu_paused) gpu_function(1,gpu,nullptr,nullptr);
    if (audio_paused) audio->Resume();
  }
};
inline void Poll(PPCContext& context, uint8_t* base) {
  (void)context;
  if (!Enabled()) return;
  // A title can change render-thread owners during startup. Claim requests
  // once and prevent concurrent access to the retained checkpoint.
  std::unique_lock operation(operation_mutex,std::try_to_lock);
  if (!operation.owns_lock()) return;
  const std::filesystem::path directory(std::getenv("FABLE2_CHECKPOINT_DIR"));
  auto request = directory / "checkpoint-request.txt";
  if (!std::filesystem::exists(request)) return;
  std::string command;
  { std::ifstream input(request); input >> command; }
  std::filesystem::remove(request);
  if (command != "save" && command != "load") {
    Status(directory, "error Unknown checkpoint command"); return;
  }
  try {
    auto* kernel = rex::system::KernelState::shared();
    auto* current = rex::system::XThread::GetCurrentThread();
    if (!kernel || !current) throw std::runtime_error("Not on guest frame thread");
    if (command == "load" && (saved_chunks.empty() || current->thread_id() != saved_owner))
      throw std::runtime_error("No checkpoint on this frame thread");
    Status(directory, "busy Pausing subsystems at gameplay frame boundary");
    Subsystems systems(kernel->emulator()); systems.Pause();
    Status(directory,"busy Acquiring guest checkpoint barrier");
    auto global = rex::thread::global_critical_region::AcquireDirect();
    std::unique_lock lock_guard(lock_mutex);
    auto objects = kernel->object_table()->GetAllObjects();
    auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
    std::vector<std::pair<uint32_t,uint32_t>> topology;
    for (auto& object : objects) topology.emplace_back(object->handle(), object->guest_object());
    std::sort(topology.begin(), topology.end());
    Stopped stopped; stopped.ports.reserve(threads.size());
    for (auto& thread : threads) if (thread->is_guest_thread()) stopped.Stop(thread.get());
    auto* memory = kernel->memory();
    auto layout = Layout(memory);
    Status(directory,"busy Copying checkpoint payload");
    if (command == "load") {
      // Additional objects/mappings outside the saved payload can remain live.
      // Every previously referenced kernel object must retain its identity.
      if (!std::includes(topology.begin(),topology.end(),saved_objects.begin(),saved_objects.end()))
        throw std::runtime_error("A checkpoint kernel object disappeared or changed identity; refusing load");
      std::vector<Allocation> recreate;
      for (const auto& chunk : saved_chunks) {
        const auto& saved=chunk.region;
        uint64_t end=uint64_t(saved.address)+saved.size;
        bool changed=false;
        for (uint64_t at=saved.address; at<end;) {
          rex::memory::HeapAllocationInfo info{};
          if (!saved.heap->QueryRegionInfo(uint32_t(at),&info) || !info.region_size ||
              !(info.state&rex::memory::kMemoryAllocationCommit) ||
              !(info.protect&rex::memory::kMemoryProtectWrite) ||
              info.allocation_base!=saved.allocation_base || info.allocation_size!=saved.allocation_size) {
            changed=true; break;
          }
          at+=std::min(uint64_t(info.region_size),end-at);
        }
        if (changed) {
          // Recreate only a complete saved allocation that is now entirely
          // free. Existing replacement allocations/partial spans are refused.
          if (saved.address!=saved.allocation_base || saved.size!=saved.allocation_size ||
              !EntirelyFree(saved.heap,saved.allocation_base,saved.allocation_size) ||
              !EntirelyFree(chunk.allocation_heap,chunk.allocation_address,saved.allocation_size))
            throw std::runtime_error("Saved allocation was replaced/partially changed at address="+
                std::to_string(saved.address)+"; refusing load");
          recreate.push_back({chunk.allocation_heap,chunk.allocation_address,saved.allocation_size,saved.protect});
        }
      }
      // Preserve live kernel allocations, worker guest stacks/TLS and all
      // observed critical sections. Their native continuations are retained.
      std::vector<Keep> keep;
      auto preserve = [&](uint32_t address, size_t bytes) {
        if (!address || !bytes) return;
        Keep item{base + address, std::vector<uint8_t>(bytes)};
        if (!ReadChecked(item.address,item.bytes.data(),bytes))
          throw std::runtime_error("Cannot read a live synchronization allocation");
        keep.push_back(std::move(item));
      };
      auto preserve_allocation = [&](uint32_t address) {
        if (!address) return;
        rex::memory::HeapAllocationInfo info{};
        auto* heap=memory->LookupHeap(address);
        if (!heap || !heap->QueryRegionInfo(address, &info) || !info.allocation_size)
          throw std::runtime_error("Cannot preserve a live kernel allocation");
        preserve(info.allocation_base, info.allocation_size);
      };
      preserve_allocation(kernel->GetKernelGuestGlobals());
      for (auto& object : objects) {
        // Embedded game events can live within a very large game allocation.
        // Preserve their dispatch record, not the entire containing game heap.
        size_t size=sizeof(rex::system::X_DISPATCH_HEADER);
        switch (object->type()) {
          case rex::system::XObject::Type::Thread: size=sizeof(rex::system::X_KTHREAD); break;
          case rex::system::XObject::Type::Timer: size=sizeof(rex::system::X_KTIMER); break;
          case rex::system::XObject::Type::Semaphore: size=sizeof(rex::system::X_KSEMAPHORE); break;
          default: break;
        }
        preserve(object->guest_object(),size);
      }
      for (auto& thread : threads) {
        preserve_allocation(thread->tls_ptr()); preserve_allocation(thread->pcr_ptr());
        if (thread->guest_object()) {
          auto* guest=memory->TranslateVirtual<rex::system::X_KTHREAD*>(thread->guest_object());
          uint32_t limit=guest->stack_limit, top=guest->stack_base;
          if (top>limit && top-limit<64*1024*1024) preserve(limit, top-limit);
        }
      }
      for (auto address : locks) preserve(address, 28);
      // Use native heap APIs to reconcile metadata, backing pages and physical
      // aliases together. All requested spans were checked free first.
      Recreated recreated; recreated.allocations.reserve(recreate.size());
      for (const auto& allocation : recreate) recreated.Create(allocation);
      // Complete preflight before the first write. Host GPU watch protection
      // may be stricter than guest heap metadata, so check host mappings too.
      for (const auto& chunk : saved_chunks)
        if (!Writable(chunk.address,chunk.bytes.size()))
          throw std::runtime_error("Host guest mapping is write-protected; refusing load");
      for (const auto& item : keep)
        if (!Writable(item.address,item.bytes.size()))
          throw std::runtime_error("Live synchronization allocation is write-protected; refusing load");
      for (auto& chunk : saved_chunks) {
        if (mach_vm_write(mach_task_self(),reinterpret_cast<mach_vm_address_t>(chunk.address),
            reinterpret_cast<vm_offset_t>(chunk.bytes.data()),chunk.bytes.size())!=KERN_SUCCESS)
          throw std::runtime_error("Guest chunk restore failed");
      }
      for (auto& item : keep)
        if (mach_vm_write(mach_task_self(),reinterpret_cast<mach_vm_address_t>(item.address),
            reinterpret_cast<vm_offset_t>(item.bytes.data()),item.bytes.size())!=KERN_SUCCESS)
          throw std::runtime_error("Synchronization allocation restoration failed");
      if (!systems.gpu_function(3,systems.gpu,saved_gpu,&saved_gpu_size))
        throw std::runtime_error("GPU command ring restore failed");
      recreated.committed=true;
    } else {
      std::vector<Chunk> chunks;
      size_t bytes=0;
      const auto copy_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
      for (const auto& region : layout) {
        if (std::chrono::steady_clock::now()>copy_deadline) throw std::runtime_error("Checkpoint copy timed out");
        // Read-only executable/data regions cannot change, and protected zero
        // or guard pages have no checkpoint payload. Physical aliases appear
        // only through the canonical physical heap.
        if (!(region.state & rex::memory::kMemoryAllocationCommit) ||
            !(region.protect & rex::memory::kMemoryProtectWrite)) continue;
        bytes+=region.size;
        if (bytes>size_t(2)*1024*1024*1024) throw std::runtime_error("Checkpoint exceeds 2 GiB budget");
        auto* address=region.heap->TranslateRelative(region.address-region.heap->heap_base());
        auto* allocation_heap=region.heap;
        uint32_t allocation_address=region.allocation_base;
        if (region.heap==memory->GetPhysicalHeap()) {
          // Capture the original physical alias, including the E-range's
          // 4-KiB guest offset. AllocFixed through it restores both ledgers.
          for (uint32_t base_address : {0xA0000000u,0xC0000000u,0xE0000000u}) {
            auto* alias=static_cast<rex::memory::PhysicalHeap*>(memory->LookupHeap(base_address));
            uint32_t offset=alias->GetPhysicalAddress(alias->heap_base());
            if (region.allocation_base<offset ||
                uint64_t(region.allocation_base-offset)+region.allocation_size>alias->heap_size()) continue;
            uint32_t guest=alias->heap_base()+region.allocation_base-offset;
            rex::memory::HeapAllocationInfo info{};
            if (alias->QueryRegionInfo(guest,&info) && info.state &&
                info.allocation_base==guest && info.allocation_size==region.allocation_size) {
              allocation_heap=alias; allocation_address=guest; break;
            }
          }
        }
        Chunk chunk{address,region,allocation_heap,allocation_address,std::vector<uint8_t>(region.size)};
        if (!ReadChecked(address,chunk.bytes.data(),region.size))
          throw std::runtime_error("Guest region is not readable; refusing capture");
        chunks.push_back(std::move(chunk));
      }

      saved_gpu_size=sizeof(saved_gpu);
      if (!systems.gpu_function(2,systems.gpu,saved_gpu,&saved_gpu_size))
        throw std::runtime_error("GPU command ring capture failed");
      saved_size=bytes; saved_chunks=std::move(chunks);
      saved_objects=std::move(topology); saved_owner=current->thread_id();
    }
    // Status I/O happens after RAII resumes all guest workers/subsystems.
  } catch (const std::exception& error) {
    Status(directory, std::string("error ")+error.what()); return;
  }
  Status(directory,"ok "+command+" experimental guest-memory/frame-boundary checkpoint bytes="+std::to_string(saved_size));
}
using Import = void (*)(PPCContext&,uint8_t*);
inline void Forward(Import function,PPCContext& context,uint8_t* base,uint32_t target,const char* symbol) {
  if (!function) std::abort();
  ObserveLock(uint32_t(context.r3.u64));
  #ifdef FABLE2_CONTINUATION_NATIVE_TRACE
  fable2::continuation::native_trace::InvokeImport(target,symbol,function,context,base);
  #else
  function(context,base);
  #endif
}
} // namespace fable2::checkpoint
extern "C" void __imp__RtlEnterCriticalSection(PPCContext& c,uint8_t* b) {
  static auto original=reinterpret_cast<fable2::checkpoint::Import>(dlsym(RTLD_NEXT,"__imp__RtlEnterCriticalSection"));
  fable2::checkpoint::Forward(original,c,b,0x832B227C,"__imp__RtlEnterCriticalSection");
}
extern "C" void __imp__RtlTryEnterCriticalSection(PPCContext& c,uint8_t* b) {
  static auto original=reinterpret_cast<fable2::checkpoint::Import>(dlsym(RTLD_NEXT,"__imp__RtlTryEnterCriticalSection"));
  fable2::checkpoint::Forward(original,c,b,0x832B221C,"__imp__RtlTryEnterCriticalSection");
}
extern "C" void __imp__RtlLeaveCriticalSection(PPCContext& c,uint8_t* b) {
  static auto original=reinterpret_cast<fable2::checkpoint::Import>(dlsym(RTLD_NEXT,"__imp__RtlLeaveCriticalSection"));
  fable2::checkpoint::Forward(original,c,b,0x832B226C,"__imp__RtlLeaveCriticalSection");
}
#endif
