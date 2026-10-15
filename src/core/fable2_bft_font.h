// Fable II UI bitmap font (.bft) loader, for host overlays that should look like
// the game (src/core/fable2_turbo_notice.h).
//
// The game's UI text is drawn from bitmap fonts in
// data/language/<culture>/fonts/gui_{large,normal,small}.bft (en-uk: 32/27/24 px).
// Nothing is copied into the repo: the file is read from the user's data/ at
// runtime and decoded in memory.
//
// Layout (all big-endian), reverse-engineered 2026-10-07:
//   0x00 u32 version (1)   0x04 u32 pixel size     0x08 u32 (400)  0x0C u32 0
//   0x10 u8  line height   0x11 u32 ascent         0x15 u32 glyph count N
//   0x19 N x 22-byte glyph records:
//          f32 u0, v0, u1, v1 (atlas UV rect)  i16 x_offset, width, advance
//   then u16 first, u16 last, (last-first) x u16 codepoint -> glyph index
//        (0x3434 = no glyph)
//   then a 0x58-byte texture header: +0x10 u32 width, +0x14 u32 height
//   then width*height/2 bytes of BC4 (DXT5A, one alpha channel), Xbox 360
//        tiled layout, 16-bit byte-swapped (k_8in16).
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fable2::bftfont {

struct Glyph {
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  int16_t x_offset = 0, width = 0, advance = 0;
};

struct Font {
  uint32_t pixel_size = 0;
  uint32_t line_height = 0;
  uint32_t ascent = 0;
  uint32_t tex_width = 0, tex_height = 0;
  std::vector<Glyph> glyphs;
  uint32_t map_first = 0;
  std::vector<uint16_t> map;   // codepoint - map_first -> glyph index
  std::vector<uint8_t> rgba;   // tex_width * tex_height * 4 (white, alpha = coverage)

  const Glyph* Find(uint32_t cp) const {
    if (cp < map_first || cp - map_first >= map.size()) return nullptr;
    const uint16_t g = map[cp - map_first];
    return g < glyphs.size() ? &glyphs[g] : nullptr;
  }
};

namespace detail {

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline float bef(const uint8_t* p) {
  const uint32_t u = be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Xbox 360 2D tiled offset (bytes) of block (x, y); `width` in blocks,
// `bpb` bytes per block. Same formula as Xenia's texture_util.
inline uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpb) {
  const uint32_t aligned = (width + 31) & ~31u;
  const uint32_t log_bpb = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpb + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb;
  const uint32_t off = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((off & ~0x1FFu) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F);
}

inline void DecodeBC4(const uint8_t* b, uint8_t out[16]) {
  uint8_t pal[8];
  pal[0] = b[0];
  pal[1] = b[1];
  if (pal[0] > pal[1]) {
    for (int i = 1; i < 7; ++i) pal[1 + i] = uint8_t(((7 - i) * pal[0] + i * pal[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) pal[1 + i] = uint8_t(((5 - i) * pal[0] + i * pal[1]) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

}  // namespace detail

// Parses and decodes a .bft file. Returns false (with `error`) on any mismatch.
inline bool Load(const std::filesystem::path& path, Font& font, std::string& error) {
  using namespace detail;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path.string();
    return false;
  }
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (d.size() < 0x19 || be32(&d[0]) != 1) {
    error = "not a version-1 .bft";
    return false;
  }
  font.pixel_size = be32(&d[4]);
  font.line_height = d[0x10];
  font.ascent = be32(&d[0x11]);
  const uint32_t count = be32(&d[0x15]);
  size_t pos = 0x19;
  if (count == 0 || count > 20000 || pos + size_t(count) * 22 + 4 > d.size()) {
    error = "bad glyph count";
    return false;
  }
  font.glyphs.resize(count);
  for (auto& g : font.glyphs) {
    const uint8_t* r = &d[pos];
    g.u0 = bef(r);
    g.v0 = bef(r + 4);
    g.u1 = bef(r + 8);
    g.v1 = bef(r + 12);
    g.x_offset = int16_t(be16(r + 16));
    g.width = int16_t(be16(r + 18));
    g.advance = int16_t(be16(r + 20));
    pos += 22;
  }
  const uint32_t first = be16(&d[pos]), last = be16(&d[pos + 2]);
  pos += 4;
  if (last < first || pos + size_t(last - first) * 2 + 0x58 > d.size()) {
    error = "bad codepoint map";
    return false;
  }
  font.map_first = first;
  font.map.resize(last - first);
  for (auto& m : font.map) {
    m = be16(&d[pos]);
    pos += 2;
  }
  const uint8_t* th = &d[pos];
  if (be32(th) != 0xFFFFFFFEu) {
    error = "bad texture header";
    return false;
  }
  const uint32_t w = be32(th + 0x10), h = be32(th + 0x14);
  pos += 0x58;
  if (w == 0 || h == 0 || (w & 3) || (h & 3) || w > 4096 || h > 4096 ||
      pos + size_t(w) * h / 2 > d.size()) {
    error = "bad texture size";
    return false;
  }
  font.tex_width = w;
  font.tex_height = h;
  font.rgba.assign(size_t(w) * h * 4, 255);
  const uint8_t* tex = &d[pos];
  const uint32_t bw = w / 4, bh = h / 4;
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint32_t off = TiledOffset(bx, by, bw, 8);
      if (size_t(off) + 8 > size_t(w) * h / 2) continue;
      uint8_t blk[8];
      for (int i = 0; i < 8; ++i) blk[i] = tex[off + (i ^ 1)];  // 8-in-16 swap
      uint8_t a[16];
      DecodeBC4(blk, a);
      for (int i = 0; i < 16; ++i) {
        const size_t px = size_t(by * 4 + i / 4) * w + bx * 4 + i % 4;
        font.rgba[px * 4 + 3] = a[i];
      }
    }
  }
  return true;
}

// Decodes one UTF-8 code point and advances `s`.
inline uint32_t NextCodepoint(const char*& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  uint32_t cp = *p++;
  int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
  if (extra) cp &= (0x3F >> extra);
  while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
  s = reinterpret_cast<const char*>(p);
  return cp;
}

}  // namespace fable2::bftfont
