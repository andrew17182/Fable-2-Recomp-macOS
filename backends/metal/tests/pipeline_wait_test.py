"""Exercise the production pipeline wait with delayed success/failure publishers."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / 'port/rex/graphics/metal/metal_pipeline_cache.h').read_text()
source = (root / 'port/rex/graphics/metal/metal_pipeline_cache.cc').read_text()
draw = (root / 'port/rex/graphics/metal/metal_command_processor.cc').read_text()
start = draw.index('      const bool preserve_morph_draw =')
selection = draw[start:draw.index(';', start) + 1]
start = header.index('    MTL::RenderPipelineState* WaitForCreation() const {')
method = header[start:header.index('\n    }', start) + len('\n    }')]
publications = []
for name in ('raw_handle', 'handle'):
    start = re.search(r'\b' + name + r'->creation_complete\.store\(', source).start()
    end = source.index(f'{name}->creation_complete.notify_all();', start)
    end += len(f'{name}->creation_complete.notify_all();')
    publications.append(source[start:end].replace(name + '->', 'handle->'))

harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
using namespace std::chrono_literals;
namespace MTL { struct RenderPipelineState { int value = 42; }; }
struct Shader { uint64_t hash; uint64_t ucode_data_hash() const { return hash; } };
#define REXCVAR_GET(name) enabled
bool Preserve(bool enabled, Shader* pixel_shader) {
  SELECTION
  return preserve_morph_draw;
}
struct Handle {
  std::atomic<MTL::RenderPipelineState*> state{nullptr};
  std::atomic<bool> creation_complete{false};
  METHOD
};
void Publish(Handle* handle, MTL::RenderPipelineState* result) {
  handle->state.store(result, std::memory_order_release);
  PUBLICATION
}
int main() {
  Shader morph{UINT64_C(0xE2120FEAFA4CF565)}, ordinary{1};
  assert(Preserve(true, &morph));
  assert(!Preserve(false, &morph));
  assert(!Preserve(true, &ordinary));
  assert(!Preserve(true, nullptr));
  MTL::RenderPipelineState pipeline;
  // Ready pipelines must not wait, even before completion bookkeeping ends.
  Handle ready;
  ready.state.store(&pipeline);
  assert(ready.WaitForCreation() == &pipeline);
  for (bool success : {false, true}) {
    Handle handle;
    std::promise<void> entered;
    auto waiter = std::async(std::launch::async, [&] {
      entered.set_value();
      return handle.WaitForCreation();
    });
    entered.get_future().wait();
    assert(waiter.wait_for(25ms) == std::future_status::timeout);
    Publish(&handle, success ? &pipeline : nullptr);
    assert(waiter.wait_for(2s) == std::future_status::ready);
    assert(waiter.get() == (success ? &pipeline : nullptr));
    // Completed failures and successes must also work for later callers.
    assert(handle.WaitForCreation() == (success ? &pipeline : nullptr));
  }
}
'''.replace('METHOD', method).replace('SELECTION', selection)
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    for publication in publications:
        (path / 'test.cpp').write_text(harness.replace('PUBLICATION', publication))
        subprocess.run(['clang++', '-std=c++20', '-pthread', str(path / 'test.cpp'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True, timeout=10)
print('Pipeline waits passed: ready, pending success/failure, and completed failure; both publication paths.')
