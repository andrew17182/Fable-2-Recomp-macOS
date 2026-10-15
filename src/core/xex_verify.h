// fable_2 - default.xex startup integrity check
//
// This build was recompiled against a specific original default.xex (the
// game executable whose switch tables / function layout were reverse
// engineered). If the user's copy differs, the recompiled code will
// misbehave in unpredictable ways, so at startup we verify the file's
// SHA-256 and refuse to launch on mismatch.
//
// Once a correct hash has been confirmed, a marker file
// (cache/default.xex.sha256) records the hash together with the file's
// size and last-write time. On future starts the marker is read and, as
// long as the file's size + mtime still match, the full-file hash pass is
// skipped. If the file ever changes (e.g. a different xex was dropped in),
// the hash is recomputed and the marker updated.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fable2::xexverify {

// SHA-256 of the default.xex this build was recompiled against.
inline constexpr char kExpectedSha256[] =
    "88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662";

namespace detail {

// Minimal self-contained SHA-256 (no third-party dependency).
struct Sha256 {
  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block_[64];
  uint64_t total_len = 0;
  size_t block_len = 0;

  static constexpr uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
  }

  void Process(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
             (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + K[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + mj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  void Update(const uint8_t* data, size_t n) {
    total_len += n;
    while (n > 0) {
      size_t take = 64 - block_len;
      if (take > n) take = n;
      std::memcpy(block_ + block_len, data, take);
      block_len += take;
      data += take;
      n -= take;
      if (block_len == 64) {
        Process(block_);
        block_len = 0;
      }
    }
  }

  void Final(uint8_t out[32]) {
    const uint64_t bits = total_len * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (block_len != 56) Update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = uint8_t(bits >> (56 - 8 * i));
    Update(lenb, 8);
    for (int i = 0; i < 8; i++) {
      out[i * 4] = uint8_t(h_[i] >> 24);
      out[i * 4 + 1] = uint8_t(h_[i] >> 16);
      out[i * 4 + 2] = uint8_t(h_[i] >> 8);
      out[i * 4 + 3] = uint8_t(h_[i]);
    }
  }
};

}  // namespace detail

// SHA-256 of a whole file, returned as lowercase hex. Returns false if the
// file cannot be read.
inline bool Sha256File(const std::filesystem::path& path, std::string& hex_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  detail::Sha256 sha;
  uint8_t buf[65536];
  while (in) {
    in.read(reinterpret_cast<char*>(buf), sizeof(buf));
    const std::streamsize n = in.gcount();
    if (n <= 0) break;
    sha.Update(buf, size_t(n));
  }
  uint8_t digest[32];
  sha.Final(digest);
  static const char* digits = "0123456789abcdef";
  hex_out.clear();
  hex_out.reserve(64);
  for (uint8_t b : digest) {
    hex_out.push_back(digits[b >> 4]);
    hex_out.push_back(digits[b & 0xf]);
  }
  return true;
}

// "Verified once" marker: the expected hash plus the file size and
// last-write time it was verified against. Lets future starts skip the
// full hash pass while still catching a replaced default.xex cheaply.
struct Marker {
  bool ok = false;
  std::string hash;
  uint64_t size = 0;
  uint64_t mtime = 0;
};

inline bool ReadMarker(const std::filesystem::path& path, Marker& m) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "hash") m.hash = value;
    else if (key == "size") m.size = strtoull(value.c_str(), nullptr, 10);
    else if (key == "mtime") m.mtime = strtoull(value.c_str(), nullptr, 10);
  }
  m.ok = !m.hash.empty() && m.hash == kExpectedSha256;
  return m.ok;
}

inline void WriteMarker(const std::filesystem::path& path, const Marker& m) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  out << "hash=" << m.hash << "\n"
      << "size=" << m.size << "\n"
      << "mtime=" << m.mtime << "\n";
}

enum class Result { VerifiedCached, VerifiedFresh, Mismatch, ReadFailed };

struct Outcome {
  Result result = Result::ReadFailed;
  std::string actual_hash;
  uint64_t size = 0;
  uint64_t mtime = 0;
};

// Verify xex_path against kExpectedSha256, using marker_path to skip the
// hash pass when a previous start already verified this exact file.
inline Outcome Check(const std::filesystem::path& xex_path,
                     const std::filesystem::path& marker_path) {
  Outcome o;
  if (!std::filesystem::is_regular_file(xex_path)) return o;
  std::error_code ec;
  const uint64_t size = uint64_t(std::filesystem::file_size(xex_path, ec));
  if (ec) return o;
  auto last_write = std::filesystem::last_write_time(xex_path, ec);
  if (ec) return o;
  o.size = size;
  o.mtime =
      uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                   last_write.time_since_epoch())
                   .count());

  // Fast path: a previous start verified this exact file (hash, size and
  // mtime all match) -> skip the full-file hash pass.
  Marker m;
  if (ReadMarker(marker_path, m) && m.size == o.size && m.mtime == o.mtime) {
    o.result = Result::VerifiedCached;
    o.actual_hash = m.hash;
    return o;
  }

  if (!Sha256File(xex_path, o.actual_hash)) return o;  // ReadFailed
  if (o.actual_hash == kExpectedSha256) {
    WriteMarker(marker_path, Marker{true, o.actual_hash, o.size, o.mtime});
    o.result = Result::VerifiedFresh;
  } else {
    o.result = Result::Mismatch;
  }
  return o;
}

}  // namespace fable2::xexverify
