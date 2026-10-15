// Reopen populated caches through the production loader. In particular, macOS
// opens a+b streams at EOF; valid entries must survive initialization.
#include "rex/graphics/shader_storage.h"
#include <cassert>

using namespace rex::graphics;
struct Description {
  uint64_t vertex_shader_hash, vertex_shader_modification;
  uint64_t pixel_shader_hash, pixel_shader_modification;
};
struct Stored {
  Description description;
  uint64_t description_hash;
};

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path root(argv[1]);
  assert(EnsureShaderStorageDirectoriesExist(root));
  const auto shaders = GetShaderStorageFilePath(root, 1);
  const auto pipelines = GetShaderStorageShareableRoot(root) / "00000001.test.xpso";
  uint32_t ucode[] = {0x12345678, 0x9abcdef0};
  const uint64_t hash = XXH3_64bits(ucode, sizeof(ucode));
  ShaderStoredHeader entry{};
  entry.ucode_data_hash = hash;
  entry.ucode_dword_count = 2;
  entry.type = xenos::ShaderType::kVertex;
  FILE* file = fopen(shaders.c_str(), "wb");
  assert(file && WriteShaderStorageHeader(file));
  assert(fwrite(&entry, sizeof(entry), 1, file) == 1);
  assert(fwrite(ucode, sizeof(ucode), 1, file) == 1);
  fclose(file);
  Stored stored{{hash, 0, 0, 0}, 0};
  stored.description_hash = XXH3_64bits(&stored.description, sizeof(stored.description));
  PipelineStorageFileHeader header{kPipelineStorageMagic, 123, rex::byte_swap(uint32_t(1))};
  file = fopen(pipelines.c_str(), "wb");
  assert(file);
  assert(fwrite(&header, sizeof(header), 1, file) == 1);
  assert(fwrite(&stored, sizeof(stored), 1, file) == 1);
  fclose(file);
  const auto shader_size = std::filesystem::file_size(shaders);
  const auto pipeline_size = std::filesystem::file_size(pipelines);
  for (int pass = 0; pass < 2; ++pass) {
    ShaderStorageWriter<Stored> writer;
    std::vector<Stored> loaded;
    unsigned shader_count = 0, translation_count = 0;
    assert(writer.InitializeShaderStorage(root, 1, {".test.xpso", 123, 1},
        [&](auto type, const uint32_t* data, uint32_t count, uint64_t loaded_hash) {
          assert(type == entry.type && count == 2 && loaded_hash == hash);
          assert(memcmp(data, ucode, sizeof(ucode)) == 0);
          ++shader_count;
          return true;
        }, [&](const auto& translations) {
          translation_count = unsigned(translations.size());
        }, loaded));
    assert(shader_count == 1 && translation_count == 1 && loaded.size() == 1);
    assert(loaded[0].description_hash == stored.description_hash);
    writer.ShutdownShaderStorage();
    assert(std::filesystem::file_size(shaders) == shader_size);
    assert(std::filesystem::file_size(pipelines) == pipeline_size);
  }
}
