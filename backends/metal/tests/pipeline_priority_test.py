"""Exercise production demand promotion with a queued background workload."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'port/rex/graphics/metal/metal_pipeline_cache.cc').read_text()
start = source.index('MTL::RenderPipelineState* MetalPipelineCache::WaitForPipeline(')
method = source[start:source.index('\nvoid MetalPipelineCache::CreationThread', start)]
header = (root / 'port/rex/graphics/metal/metal_pipeline_cache.h').read_text()
start = header.index('    MTL::RenderPipelineState* WaitForCreation() const {')
wait = header[start:header.index('\n    }', start) + 6]
code = r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
namespace MTL { struct RenderPipelineState {}; }
// Hitch-trace scope (metal_hitch_trace): timing only, a no-op double here.
namespace hitch { enum Kind { kPsoWait }; struct Scope { explicit Scope(Kind) {} }; }
class MetalPipelineCache {
public:
 struct PipelineHandle {
  std::atomic<MTL::RenderPipelineState*> state{nullptr};
  std::atomic<bool> creation_complete{false};
  uint8_t priority = 0;
  WAIT_METHOD
 };
 struct PipelineCreationRequest { PipelineHandle* handle; };
 struct Compare {
  bool operator()(const PipelineCreationRequest& a, const PipelineCreationRequest& b) const {
   return a.handle->priority < b.handle->priority;
  }
 };
 std::priority_queue<PipelineCreationRequest,std::vector<PipelineCreationRequest>,Compare> creation_queue_;
 std::mutex creation_request_lock_;
 std::condition_variable creation_request_cond_;
 MTL::RenderPipelineState* WaitForPipeline(PipelineHandle* handle);
};
PRODUCTION_METHOD
int main() {
 for (bool success : {true, false}) {
  MetalPipelineCache cache;
  MetalPipelineCache::PipelineHandle background[300], demand;
  for (auto& h : background) cache.creation_queue_.push({&h});
  cache.creation_queue_.push({&demand});
  MTL::RenderPipelineState result;
  auto consumer = std::async(std::launch::async,[&]{ return cache.WaitForPipeline(&demand); });
  {
   std::unique_lock lock(cache.creation_request_lock_);
   bool promoted = cache.creation_request_cond_.wait_for(lock,std::chrono::seconds(2),[&]{return cache.creation_queue_.top().handle == &demand;});
   assert(promoted);
   cache.creation_queue_.pop();
   assert(cache.creation_queue_.size() == 300);
  }
  demand.state.store(success ? &result : nullptr, std::memory_order_release);
  demand.creation_complete.store(true, std::memory_order_release);
  demand.creation_complete.notify_all();
  assert(consumer.get() == (success ? &result : nullptr));
  // A completed handle needs no queue entry and must not disturb other work.
  assert(cache.WaitForPipeline(&demand) == (success ? &result : nullptr));
  assert(cache.creation_queue_.size() == 300);
 }
}
'''.replace('WAIT_METHOD', wait).replace('PRODUCTION_METHOD', method)
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(code)
    exe = Path(tmp) / 'test'
    subprocess.run(['clang++', '-std=c++20', '-pthread', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
print('Demand priority passed: 300 background jobs, success/failure, no duplicates, completed handles.')
