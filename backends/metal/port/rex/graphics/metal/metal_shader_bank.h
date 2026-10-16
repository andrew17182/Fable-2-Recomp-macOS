#pragma once
// [bank-prep] Reader for the game's shader bank (data/Shaders/ShadersRelease.sbk on the player's own disc;
// docs/shader-precompile-20261015.md). Nothing game-derived ships with the port: at install the headless shader
// prep (FABLE2_SHADER_PREP) reads the bank from the extracted game files and translates / compiles its shaders.
//
// Format (also tools/fable2_shader_bank.py): "ShaderBankFile" header, a constant-name table, then a count N, N
// decompressed sizes, N stream offsets, and per entry (u32, u32 compressed size, zlib stream without a final
// block). Each decompressed entry holds D3D shader containers (big-endian): magic 0x102A1100 (pixel) or
// 0x102A1101 (vertex), +4 offset of the data block, +8 data block size, +24 offset of the shader info block.
// The data block is the literal constants followed by the microcode; the info block's second word is the
// microcode size; words 2, 3 and 6 are the SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC and SQ_INTERPOLATOR_CNTL values D3D
// sets with the shader (param gen, its interpolator, centroid sampling). The microcode is what the game hands the GPU (pixel shaders byte-identical; most vertex
// shaders get their vertex-fetch instructions patched by D3D at bind time).
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::graphics::metal::bank {

struct BankShader {
  bool pixel = false;
  std::vector<uint32_t> ucode;  // guest (big-endian) byte order, as in guest memory
  // Register values from the container's shader info block (0 when the block is too short).
  uint32_t sq_program_cntl = 0;
  uint32_t sq_context_misc = 0;
  uint32_t sq_interpolator_cntl = 0;
};

inline uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline bool Inflate(const uint8_t* src, size_t size, size_t expected, std::vector<uint8_t>* out) {
  out->assign(expected, 0);
  z_stream z = {};
  if (inflateInit(&z) != Z_OK) return false;
  z.next_in = const_cast<Bytef*>(src);
  z.avail_in = uInt(size);
  z.next_out = out->data();
  z.avail_out = uInt(expected);
  // The streams have no final block: Z_SYNC_FLUSH output is complete once every input byte is consumed.
  const int r = inflate(&z, Z_SYNC_FLUSH);
  const size_t produced = expected - z.avail_out;
  inflateEnd(&z);
  return (r == Z_OK || r == Z_STREAM_END || r == Z_BUF_ERROR) && produced == expected;
}

// Every shader container of one bank file. Returns false (and an error) when the file is missing or not a bank.
inline bool ReadBank(const std::filesystem::path& path, std::vector<BankShader>* out, std::string* error) {
  out->clear();
  std::vector<uint8_t> d;
  {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      *error = "cannot open " + path.string();
      return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    d.resize(n > 0 ? size_t(n) : 0);
    const size_t got = d.empty() ? 0 : std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if (got != d.size()) {
      *error = "short read of " + path.string();
      return false;
    }
  }
  static const char kMagic[] = "ShaderBankFile";
  if (d.size() < 64 || std::memcmp(d.data(), kMagic, sizeof(kMagic) - 1) != 0) {
    *error = path.string() + " is not a ShaderBankFile";
    return false;
  }
  // The tables sit right before the first zlib stream (78 DA); their count is stored before the size table.
  for (size_t first = 0; first + 2 < d.size(); ++first) {
    if (d[first] != 0x78 || d[first + 1] != 0xDA) continue;
    for (size_t n = 1; n < 4096; ++n) {
      if (first < 8 + 8 * n + 4 + 16) break;
      const size_t c = first - 8, b = c - 4 * n, a = b - 4 * n - 4;
      if (ReadBe32(&d[a]) != n || ReadBe32(&d[b]) != 0) continue;
      std::vector<uint8_t> entry;
      bool ok = true;
      for (size_t i = 0; i < n && ok; ++i) {
        const uint32_t expected = ReadBe32(&d[a + 4 + 4 * i]);
        const size_t s = c + ReadBe32(&d[b + 4 * i]);
        if (s + 8 > d.size()) {
          ok = false;
          break;
        }
        const uint32_t csize = ReadBe32(&d[s + 4]);
        if (s + 8 + csize > d.size() || !Inflate(&d[s + 8], csize, expected, &entry)) {
          ok = false;
          break;
        }
        for (size_t q = 0; q + 64 <= entry.size(); q += 4) {
          const uint32_t magic = ReadBe32(&entry[q]);
          if (magic != 0x102A1100u && magic != 0x102A1101u) continue;
          const uint32_t data_off = ReadBe32(&entry[q + 4]);
          const uint32_t data_size = ReadBe32(&entry[q + 8]);
          const uint32_t info_off = ReadBe32(&entry[q + 24]);
          if (uint64_t(q) + info_off + 8 > entry.size() ||
              uint64_t(q) + data_off + data_size > entry.size()) {
            continue;
          }
          const uint32_t ucode_size = ReadBe32(&entry[q + info_off + 4]);
          if (!ucode_size || (ucode_size & 3) || ucode_size > data_size) continue;
          BankShader shader;
          shader.pixel = magic == 0x102A1100u;
          shader.ucode.resize(ucode_size / 4);
          std::memcpy(shader.ucode.data(), &entry[q + data_off + data_size - ucode_size], ucode_size);
          if (uint64_t(q) + info_off + 28 <= entry.size()) {
            shader.sq_program_cntl = ReadBe32(&entry[q + info_off + 8]);
            shader.sq_context_misc = ReadBe32(&entry[q + info_off + 12]);
            shader.sq_interpolator_cntl = ReadBe32(&entry[q + info_off + 24]);
          }
          out->push_back(std::move(shader));
        }
      }
      if (ok) return true;
      out->clear();
    }
  }
  *error = path.string() + ": no entry table found";
  return false;
}

}  // namespace rex::graphics::metal::bank
