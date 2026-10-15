// [tex-rename] Texture renaming for loads into textures the open command
// buffer already sampled (docs/tex-rename-20261013.md).
//
// Without it, a texture load whose destination a draw of the open command
// buffer has sampled cannot go to the pre-commit command buffer (it would run
// before that draw) and cannot run inside the render pass, so the pass ends
// ("texture:sampled_current"). A native driver renames the resource instead:
// the upload goes into fresh memory and later draws bind it.
//
// Here the rename happens when the draw that needs the load is prepared,
// before its bindless descriptor indices are written: the MetalTexture keeps
// its identity (cache entry, bindings, RTT registry), only its backing
// MTL::Texture is swapped for a pooled one of the same descriptor. The old
// backing's bindless slots are retired (freed after the submission
// completes), so every draw encoded before keeps sampling the old contents
// through them; the draw being prepared and every later one get new slots.
// The load then writes the fresh backing, which nothing has sampled, so it can
// go to the pre-commit command buffer and the pass stays open.
//
// Only full loads (every level) with no possible RTT keep are renamed, so the
// new backing never needs the old contents. If its load does not write it
// anyway (a failure, a keep, a dropped draw), "settle" copies the old backing
// into it at the load's position (ending the pass), which is the state the
// split path would have.
//
// Switches: FABLE2_TEX_RENAME (default on; =0 is the old split path),
// FABLE2_TEX_RENAME_NOOP (default on: planned loads that write nothing take
// the pre-commit route), FABLE2_TEX_UNCHANGED (default off: skip reloads of
// unscaled textures nothing wrote, metal_resolve_to_texture.cc),
// FABLE2_TEX_RENAME_MB (memory cap for pooled + in-flight old backings,
// default 128), FABLE2_TEX_RENAME_VERIFY=1 (re-run the split path's load into
// the old backing at the original position and compare bytes),
// FABLE2_TEX_RENAME_VERIFY_BREAK=1 (negative control: the new backing's level
// 0 is overwritten with the reference shifted by one 4-texel block).

#include "rex/graphics/metal/metal_texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_pass_trace.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/logging.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {

bool EnvFlag(const char* name, bool default_value) {
  const char* v = std::getenv(name);
  if (!v || !v[0]) return default_value;
  return v[0] != '0';
}

uint64_t RenameCapBytes() {
  static const uint64_t cap = [] {
    const char* v = std::getenv("FABLE2_TEX_RENAME_MB");
    uint64_t mb = v ? std::strtoull(v, nullptr, 10) : 128;
    return mb << 20;
  }();
  return cap;
}

constexpr uint64_t kRenameMaxTextureBytes = uint64_t(64) << 20;
// Pooled backings unused for this many submissions (~3 per frame) are freed.
constexpr uint64_t kRenamePoolIdleSubmissions = 1024;

bool SameDescriptor(MTL::Texture* a, MTL::Texture* b) {
  if (a->textureType() != b->textureType() ||
      a->pixelFormat() != b->pixelFormat() || a->width() != b->width() ||
      a->height() != b->height() || a->depth() != b->depth() ||
      a->mipmapLevelCount() != b->mipmapLevelCount() ||
      a->arrayLength() != b->arrayLength() ||
      a->sampleCount() != b->sampleCount() || a->usage() != b->usage() ||
      a->storageMode() != b->storageMode()) {
    return false;
  }
  MTL::TextureSwizzleChannels sa = a->swizzle(), sb = b->swizzle();
  return sa.red == sb.red && sa.green == sb.green && sa.blue == sb.blue &&
         sa.alpha == sb.alpha;
}

// Block size and bytes per block for the verify readback (0: unsupported).
struct BlockInfo {
  uint32_t w = 1, h = 1, bytes = 0;
};
BlockInfo GetBlockInfo(MTL::PixelFormat f) {
  switch (f) {
    case MTL::PixelFormatA8Unorm:
    case MTL::PixelFormatR8Unorm:
    case MTL::PixelFormatR8Snorm:
    case MTL::PixelFormatR8Uint:
    case MTL::PixelFormatR8Sint:
      return {1, 1, 1};
    case MTL::PixelFormatR16Unorm:
    case MTL::PixelFormatR16Snorm:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatRG8Unorm:
    case MTL::PixelFormatRG8Snorm:
    case MTL::PixelFormatRG8Uint:
    case MTL::PixelFormatB5G6R5Unorm:
    case MTL::PixelFormatABGR4Unorm:
    case MTL::PixelFormatBGR5A1Unorm:
    case MTL::PixelFormatA1BGR5Unorm:
    case MTL::PixelFormatDepth16Unorm:
      return {1, 1, 2};
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatRGBA8Snorm:
    case MTL::PixelFormatRGBA8Uint:
    case MTL::PixelFormatRGBA8Sint:
    case MTL::PixelFormatBGRA8Unorm:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatRG16Unorm:
    case MTL::PixelFormatRG16Snorm:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRG16Sint:
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatR32Float:
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatRGB10A2Unorm:
    case MTL::PixelFormatRG11B10Float:
    case MTL::PixelFormatDepth32Float:
      return {1, 1, 4};
    case MTL::PixelFormatRGBA16Unorm:
    case MTL::PixelFormatRGBA16Snorm:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatRGBA16Sint:
    case MTL::PixelFormatRGBA16Float:
    case MTL::PixelFormatRG32Float:
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRG32Sint:
      return {1, 1, 8};
    case MTL::PixelFormatRGBA32Float:
    case MTL::PixelFormatRGBA32Uint:
    case MTL::PixelFormatRGBA32Sint:
      return {1, 1, 16};
    case MTL::PixelFormatBC1_RGBA:
    case MTL::PixelFormatBC4_RUnorm:
    case MTL::PixelFormatBC4_RSnorm:
      return {4, 4, 8};
    case MTL::PixelFormatBC2_RGBA:
    case MTL::PixelFormatBC3_RGBA:
    case MTL::PixelFormatBC5_RGUnorm:
    case MTL::PixelFormatBC5_RGSnorm:
      return {4, 4, 16};
    default:
      return {};
  }
}

struct VerifyStats {
  std::atomic<uint64_t> ok{0}, mismatch{0}, skipped{0}, bytes{0};
};
VerifyStats& Stats() {
  static VerifyStats s;
  return s;
}

}  // namespace

bool MetalTextureCache::TexRenameEnabled() {
  static const bool on = [] {
    const bool e = EnvFlag("FABLE2_TEX_RENAME", true);
    REXLOG_INFO(
        "[tex-rename] {} (FABLE2_TEX_RENAME): full reloads of a texture the "
        "open command buffer sampled go into a fresh backing instead of "
        "ending the render pass",
        e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::TexRenameVerify() {
  static const bool on = [] {
    const bool e = TexRenameEnabled() && EnvFlag("FABLE2_TEX_RENAME_VERIFY", false);
    if (e) {
      REXLOG_WARN(
          "[tex-rename] FABLE2_TEX_RENAME_VERIFY=1 (diagnostic: re-runs each "
          "renamed load at its split-path position, ends the pass)");
    }
    return e;
  }();
  return on;
}

bool MetalTextureCache::TexRenameNoopEnabled() {
  static const bool on = [] {
    const bool e = TexRenameEnabled() && EnvFlag("FABLE2_TEX_RENAME_NOOP", true);
    if (TexRenameEnabled()) {
      REXLOG_INFO(
          "[tex-rename] no-op loads {} (FABLE2_TEX_RENAME_NOOP): planned loads "
          "of textures already up to date, or fully kept by RTT, do not end "
          "the render pass",
          e ? "on" : "off");
    }
    return e;
  }();
  return on;
}

bool MetalTextureCache::PlannedLoadIsNoop(MetalTexture& texture,
                                          bool load_base, bool load_mips) {
  // Up to date since the plan was made (an earlier flush loaded it): the
  // execution skips it (TextureCache::LoadTexturesData re-reads the flags).
  const bool base = load_base && texture.base_outdated_lockless();
  const bool mips = load_mips && texture.mips_outdated_lockless();
  if (!base && !mips) {
    ++UploadTrace().trn_noop_stale;
    return true;
  }
  // [tex-unchanged] kept whole: no write over its exact ranges since.
  if (texture.nx_tracked_ && NxKeepEnabled() && !NxVerify()) {
    auto global_lock = AcquireGlobalLock();
    if (texture.nx_valid_) {
      ++UploadTrace().trn_noop_keep;
      return true;
    }
  }
  // Fully kept by RTT (RttTryKeep returns without writing).
  if (!texture.rtt_tracked_ || RttVerify() ||
      texture.key().scaled_resolve != IsDrawResolutionScaled() ||
      !RttKeepActive()) {
    return false;
  }
  const bool mips_on = RttMipsKeepActive();
  if (!base && !mips_on) return false;
  auto global_lock = AcquireGlobalLock();
  const bool keep_base = base && texture.rtt_valid_;
  const bool keep_mips = mips && mips_on && texture.rtt_mips_valid_;
  if ((!base || keep_base) && (!mips || keep_mips)) {
    ++UploadTrace().trn_noop_keep;
    return true;
  }
  return false;
}

bool MetalTextureCache::NxVerify() {
  static const bool on = [] {
    const bool e = NxKeepEnabled() && EnvFlag("FABLE2_TEX_UNCHANGED_VERIFY", false);
    if (e) {
      REXLOG_WARN("[tex-unchanged] FABLE2_TEX_UNCHANGED_VERIFY=1: kept loads "
                  "are re-run as reference loads and compared ([rtt] verify)");
    }
    return e;
  }();
  return on;
}

static bool TexRenameVerifyBreak() {
  static const bool on = EnvFlag("FABLE2_TEX_RENAME_VERIFY_BREAK", false);
  return on;
}

uint64_t MetalTextureCache::TexRenameBytes(MTL::Texture* t) {
  return t ? uint64_t(t->allocatedSize()) : 0;
}

MTL::Texture* MetalTextureCache::MetalTexture::RenameBacking(
    MTL::Texture* new_texture, std::vector<MTL::Texture*>& old_views_out) {
  MTL::Texture* old = metal_texture_;
  for (auto& entry : swizzled_view_cache_) {
    if (entry.second) old_views_out.push_back(entry.second);
  }
  swizzled_view_cache_.clear();
  if (direct_write_view_) {
    old_views_out.push_back(direct_write_view_);
    direct_write_view_ = nullptr;
  }
  direct_write_view_texel_bytes_ = 0;
  direct_write_view_failed_ = false;
  MetalCommandProcessor* cp = texture_cache_.command_processor_;
  // Retired, not freed: draws already encoded index these heap entries,
  // which keep pointing at the old backing until the submission completes.
  if (bindless_srv_index_ != UINT32_MAX) {
    cp->RetireViewBindlessIndex(bindless_srv_index_);
    bindless_srv_index_ = UINT32_MAX;
  }
  for (auto& [key, index] : swizzled_view_bindless_srv_indices_) {
    if (index != UINT32_MAX) cp->RetireViewBindlessIndex(index);
  }
  swizzled_view_bindless_srv_indices_.clear();
  metal_texture_ = new_texture;
  RefreshMtlProps();  // [draw-prep-lite2] same descriptor; refreshed anyway
  return old;
}

void MetalTextureCache::MetalTexture::SwapBackingForVerify(
    MTL::Texture*& texture, MTL::Texture*& direct_view) {
  std::swap(metal_texture_, texture);
  RefreshMtlProps();  // [draw-prep-lite2]
  std::swap(direct_write_view_, direct_view);
  // The swapped-in direct view (if any) is rebuilt on demand.
  direct_write_view_texel_bytes_ = 0;
  direct_write_view_failed_ = false;
}

bool MetalTextureCache::LoadOtherwiseEligible(MetalTexture& texture,
                                              bool load_base, bool load_mips,
                                              bool use_cpu_source,
                                              bool ignore_invalid) {
  // Mirrors ClassifyMaterializationPlan without the sampled-current check.
  MetalSharedMemory& metal_shared_memory =
      static_cast<MetalSharedMemory&>(shared_memory());
  const TextureKey& key = texture.key();
  const uint32_t base_length =
      load_base ? rex::align(texture.GetGuestBaseSize(), UINT32_C(16)) : 0;
  const uint32_t mips_length =
      load_mips ? rex::align(texture.GetGuestMipsSize(), UINT32_C(16)) : 0;
  tex_rename_last_reasons_ = 0;
  if (texture.presented()) {
    tex_rename_last_reasons_ = 0x1000;
    return false;
  }
  const bool scaled = key.scaled_resolve != 0;
  if (!use_cpu_source && !scaled) {
    uint32_t res_reasons = 0;
    auto touched = [&](uint32_t start, uint32_t length) {
      if (!length) return;
      if (!ignore_invalid && !shared_memory().IsRangeValid(start, length)) {
        res_reasons |= 8;
      }
      res_reasons |= metal_shared_memory.RangeOpenWriteReasons(start, length);
    };
    touched(key.base_page << 12, base_length);
    touched(key.mip_page << 12, mips_length);
    if (res_reasons & ~(16u | 32u)) {
      tex_rename_last_reasons_ = res_reasons;
      return false;
    }
  }
  if (scaled) {
    uint32_t scl_reasons = 0;
    auto scaled_check = [&](uint32_t start, uint32_t guest_size,
                            uint32_t aligned_length) {
      if (!aligned_length) return;
      if (ScaledResolveRangeWrittenInOpenCommandBuffer(start, guest_size)) {
        scl_reasons |= 1;
      }
      if (!IsScaledResolveRangeResident(start, guest_size, 4)) {
        scl_reasons |= 2;
      }
      if (!ignore_invalid && !shared_memory().IsRangeValid(start, aligned_length)) {
        scl_reasons |= 4;
      }
    };
    scaled_check(key.base_page << 12, texture.GetGuestBaseSize(), base_length);
    scaled_check(key.mip_page << 12, texture.GetGuestMipsSize(), mips_length);
    if (!ScaledTexturePrecommitEnabled() || !IsDrawResolutionScaled()) {
      scl_reasons |= 8;
    }
    if (scl_reasons) {
      tex_rename_last_reasons_ = 0x100 | scl_reasons;
      return false;
    }
  }
  return true;
}

MTL::Texture* MetalTextureCache::TexRenameAcquire(MTL::Texture* like) {
  for (size_t i = 0; i < rename_pool_.size(); ++i) {
    if (SameDescriptor(rename_pool_[i].texture, like)) {
      MTL::Texture* t = rename_pool_[i].texture;
      rename_pool_bytes_ -= std::min(rename_pool_bytes_, rename_pool_[i].bytes);
      rename_pool_.erase(rename_pool_.begin() + i);
      ++UploadTrace().trn_pool_hit;
      return t;
    }
  }
  const uint64_t bytes = TexRenameBytes(like);
  if (rename_retired_bytes_ + rename_pool_bytes_ + bytes > RenameCapBytes()) {
    // Free idle pool entries of other shapes first.
    while (!rename_pool_.empty() &&
           rename_retired_bytes_ + rename_pool_bytes_ + bytes >
               RenameCapBytes()) {
      RenamePoolEntry& e = rename_pool_.front();
      rename_pool_bytes_ -= std::min(rename_pool_bytes_, e.bytes);
      e.texture->release();
      rename_pool_.erase(rename_pool_.begin());
    }
    if (rename_retired_bytes_ + bytes > RenameCapBytes()) {
      return nullptr;
    }
  }
  MTL::TextureDescriptor* d = MTL::TextureDescriptor::alloc()->init();
  d->setTextureType(like->textureType());
  d->setPixelFormat(like->pixelFormat());
  d->setWidth(like->width());
  d->setHeight(like->height());
  d->setDepth(like->depth());
  d->setMipmapLevelCount(like->mipmapLevelCount());
  d->setArrayLength(like->arrayLength());
  d->setSampleCount(like->sampleCount());
  d->setUsage(like->usage());
  d->setStorageMode(like->storageMode());
  d->setSwizzle(like->swizzle());
  MTL::Texture* t = CreateTexture(d);  // releases d
  if (t && like->label()) {
    t->setLabel(like->label());
  }
  return t;
}

void MetalTextureCache::TexRenameRetire(MTL::Texture* texture,
                                        std::vector<MTL::Texture*>&& views) {
  RenameRetired r;
  r.texture = texture;
  r.views = std::move(views);
  r.bytes = TexRenameBytes(texture);
  r.submission = command_processor_->GetCurrentSubmission();
  rename_retired_bytes_ += r.bytes;
  rename_retired_.push_back(std::move(r));
  UploadTrace().trn_retired_kb = uint32_t(rename_retired_bytes_ >> 10);
}

void MetalTextureCache::TexRenameCompleted(uint64_t completed_submission) {
  if (rename_retired_.empty() && rename_pool_.empty()) return;
  for (auto it = rename_retired_.begin(); it != rename_retired_.end();) {
    if (it->submission > completed_submission) {
      ++it;
      continue;
    }
    for (MTL::Texture* v : it->views) v->release();
    rename_retired_bytes_ -= std::min(rename_retired_bytes_, it->bytes);
    if (!it->texture) {
    } else if (rename_pool_bytes_ + it->bytes <= RenameCapBytes()) {
      rename_pool_.push_back({it->texture, it->bytes, completed_submission});
      rename_pool_bytes_ += it->bytes;
    } else {
      it->texture->release();
    }
    it = rename_retired_.erase(it);
  }
  for (auto it = rename_pool_.begin(); it != rename_pool_.end();) {
    if (it->freed_submission + kRenamePoolIdleSubmissions < completed_submission) {
      rename_pool_bytes_ -= std::min(rename_pool_bytes_, it->bytes);
      it->texture->release();
      it = rename_pool_.erase(it);
    } else {
      ++it;
    }
  }
  UploadTraceCounters& u = UploadTrace();
  u.trn_pool_kb = uint32_t(rename_pool_bytes_ >> 10);
  u.trn_retired_kb = uint32_t(rename_retired_bytes_ >> 10);
}

bool MetalTextureCache::TexRenameTry(MetalTexture& texture, bool load_base,
                                     bool load_mips, bool use_cpu_source) {
  if (!TexRenameEnabled() || !command_processor_) return false;
  MTL::Texture* cur = texture.metal_texture();
  if (!cur) return false;
  UploadTraceCounters& u = UploadTrace();
  if (texture.rename_pending_) {
    // Renamed for an earlier plan of the open queue and not loaded yet; this
    // load writes the same new backing (nothing has sampled it).
    ++u.trn_share;
    return false;
  }
  if (!command_processor_->IsTextureSampledByCurrentCommandBuffer(cur)) {
    return false;
  }
  // [tex-unchanged] a reload of a texture the open command buffer sampled:
  // track it so its next reload can be skipped when nothing wrote it.
  if (NxKeepEnabled() && !texture.nx_tracked_) NxTrack(texture);
  if (!command_processor_->TexRenamePassOpen()) {
    ++u.trn_fb_nopass;
    tex_rename_last_reasons_ = 0;
    if (TexRenameTraceEnabled()) {
      TexRenameTraceNote(fmt::format("fb nopass tex {}x{} fmt {} why {:X}",
                                     cur->width(), cur->height(),
                                     uint32_t(texture.key().format),
                                     tex_rename_last_reasons_));
    }
    return false;
  }
  if (!load_base || (!load_mips && cur->mipmapLevelCount() > 1)) {
    ++u.trn_fb_partial;
    return false;
  }
  if (texture.presented() || texture.has_3d_as_2d() ||
      cur->storageMode() != MTL::StorageModePrivate ||
      cur->textureType() == MTL::TextureType3D ||
      TexRenameBytes(cur) > kRenameMaxTextureBytes) {
    ++u.trn_fb_kind;
    return false;
  }
  {
    // A keep would leave the new backing unwritten.
    auto global_lock = AcquireGlobalLock();
    if ((texture.rtt_tracked_ &&
         (texture.rtt_valid_ || texture.rtt_mips_valid_)) ||
        (texture.nx_tracked_ && NxKeepEnabled() && texture.nx_valid_)) {
      ++u.trn_fb_keep;
      tex_rename_last_reasons_ = 0;
      return false;
    }
  }
  if (!LoadOtherwiseEligible(texture, load_base, load_mips, use_cpu_source,
                             true)) {
    if (TexRenameTraceEnabled()) {
      TexRenameTraceNote(fmt::format("fb other tex {}x{} fmt {} why {:X}",
                                     cur->width(), cur->height(),
                                     uint32_t(texture.key().format),
                                     tex_rename_last_reasons_));
    }
    ++u.trn_fb_other;
    return false;
  }
  if (const int q =
          command_processor_->TexRenameQueuedDrawsSample(cur, &texture)) {
    // In the split path the queued draws would see the new contents (the
    // flush loads before encoding them); renaming would give them the old.
    tex_rename_last_reasons_ = uint32_t(q);
    if (TexRenameTraceEnabled()) {
      TexRenameTraceNote(fmt::format("fb queued tex {}x{} fmt {} why {:X}",
                                     cur->width(), cur->height(),
                                     uint32_t(texture.key().format),
                                     tex_rename_last_reasons_));
    }
    ++(q == 1 ? u.trn_fb_queued_load : u.trn_fb_queued);
    return false;
  }
  MTL::Texture* fresh = TexRenameAcquire(cur);
  if (!fresh) {
    ++u.trn_fb_mem;
    return false;
  }
  std::vector<MTL::Texture*> views;
  MTL::Texture* old = texture.RenameBacking(fresh, views);
  old->retain();
  texture.rename_src_ = old;
  TexRenameRetire(old, std::move(views));
  texture.rename_pending_ = true;
  rename_pending_list_.push_back(&texture);
  command_processor_->TexRenameInvalidateDescriptors();
  ++u.trn_rename;
  return true;
}

void MetalTextureCache::TexRenameClearPending(MetalTexture& texture,
                                              bool load_base, bool load_mips) {
  if (!texture.rename_pending_) return;
  MTL::Texture* t = texture.metal_texture();
  if (load_base && (load_mips || (t && t->mipmapLevelCount() <= 1))) {
    texture.rename_pending_ = false;
  }
}

bool MetalTextureCache::TexRenameNeedsSettle(
    const TextureMaterializationPlan* const* plans, size_t count) const {
  if (rename_pending_list_.empty()) return false;
  for (size_t i = 0; i < count; ++i) {
    for (const auto& load : plans[i]->renamed_loads_) {
      if (!load.renamed) continue;
      const auto* t = static_cast<const MetalTexture*>(load.texture);
      if (t->rename_pending_ || (TexRenameVerify() && t->rename_src_)) {
        return true;
      }
    }
  }
  return false;
}

void MetalTextureCache::TexRenameSettleOne(MetalTexture& texture,
                                           MTL::CommandBuffer* cmd,
                                           MTL::BlitCommandEncoder*& blit) {
  if (!texture.rename_pending_ || !texture.rename_src_) return;
  if (!blit) blit = pass_trace::Blit(cmd, "tex_rename_settle");
  if (blit) {
    blit->copyFromTexture(texture.rename_src_, texture.metal_texture());
  }
  texture.rename_pending_ = false;
}

void MetalTextureCache::TexRenameSettleEncode(
    const TextureMaterializationPlan* const* plans, size_t count,
    MTL::CommandBuffer* cmd) {
  // Collect this flush's renamed textures (once each) and take them off the
  // pending list.
  std::vector<std::pair<MetalTexture*, const TextureMaterializationPlan::TextureLoad*>>
      renamed;
  for (size_t i = 0; i < count; ++i) {
    for (const auto& load : plans[i]->renamed_loads_) {
      if (!load.renamed) continue;
      auto* t = static_cast<MetalTexture*>(load.texture);
      auto it = std::find(rename_pending_list_.begin(),
                          rename_pending_list_.end(), t);
      if (it == rename_pending_list_.end()) continue;
      rename_pending_list_.erase(it);
      renamed.push_back({t, &load});
    }
  }
  if (renamed.empty()) return;
  if (cmd) FlushPendingUploadEncodersForCommandEncoderBoundary();
  MTL::BlitCommandEncoder* blit = nullptr;
  for (auto& [t, load] : renamed) {
    if (t->rename_pending_ && cmd) {
      ++UploadTrace().trn_settle;
      TexRenameSettleOne(*t, cmd, blit);
    }
  }
  if (TexRenameVerify() && cmd) {
    MTL::Device* device = command_processor_->GetMetalDevice();
    struct Pair {
      MTL::Buffer* a = nullptr;  // renamed backing
      MTL::Buffer* b = nullptr;  // split-path reference
      uint64_t length = 0;
      std::string what;
    };
    auto pairs = std::make_shared<std::vector<Pair>>();
    for (auto& [t, load] : renamed) {
      MTL::Texture* fresh = t->metal_texture();
      MTL::Texture* old = t->rename_src_;
      if (!fresh || !old) continue;
      const BlockInfo bi = GetBlockInfo(fresh->pixelFormat());
      if (!bi.bytes || fresh->textureType() == MTL::TextureType3D ||
          fresh->sampleCount() != 1) {
        Stats().skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (blit) {
        blit->endEncoding();
        blit = nullptr;
      }
      // The split path: the same load into the old backing at this point of
      // the command buffer (after every draw that sampled it).
      MTL::Texture* swap_tex = old;
      MTL::Texture* swap_view = nullptr;
      t->SwapBackingForVerify(swap_tex, swap_view);
      BeginTextureUploadBatch();
      const bool ok = TryGpuLoadTexture(
          *t, load->load_base, load->load_mips,
          load->use_cpu_source ? TextureLoadSourceMode::kCpuGuestMemory
                               : TextureLoadSourceMode::kResidentMemory);
      EndTextureUploadBatch();
      t->SwapBackingForVerify(swap_tex, swap_view);
      // swap_view now holds the direct view made for the old backing; the
      // load just encoded uses it, so it lives until the submission is done.
      if (swap_view) {
        RenameRetired r;
        r.views.push_back(swap_view);
        r.submission = command_processor_->GetCurrentSubmission();
        rename_retired_.push_back(std::move(r));
      }
      if (!ok) {
        Stats().skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      FlushPendingUploadEncodersForCommandEncoderBoundary();
      if (!blit) blit = pass_trace::Blit(cmd, "tex_rename_verify");
      if (TexRenameVerifyBreak() && fresh->width() >= 8) {
        // Negative control: the renamed backing's level 0 shifted right by
        // 4 texels (one block), as a wrong load would leave it.
        blit->copyFromTexture(old, 0, 0, MTL::Origin::Make(0, 0, 0),
                              MTL::Size::Make(fresh->width() - 4,
                                              fresh->height(), 1),
                              fresh, 0, 0, MTL::Origin::Make(4, 0, 0));
      }
      const uint32_t slices =
          fresh->textureType() == MTL::TextureTypeCube ? 6
          : fresh->textureType() == MTL::TextureTypeCubeArray
              ? uint32_t(fresh->arrayLength()) * 6
              : uint32_t(fresh->arrayLength());
      uint64_t total = 0;
      for (uint32_t level = 0; level < fresh->mipmapLevelCount(); ++level) {
        const uint32_t w = std::max<uint32_t>(uint32_t(fresh->width()) >> level, 1);
        const uint32_t h = std::max<uint32_t>(uint32_t(fresh->height()) >> level, 1);
        const uint64_t row = uint64_t((w + bi.w - 1) / bi.w) * bi.bytes;
        total += row * ((h + bi.h - 1) / bi.h) * slices;
      }
      Pair p;
      p.length = total;
      p.a = device->newBuffer(total, MTL::ResourceStorageModeShared);
      p.b = device->newBuffer(total, MTL::ResourceStorageModeShared);
      p.what = fmt::format("fmt {} {}x{}x{} lv {} base {:08X}",
                           uint32_t(t->key().format), fresh->width(),
                           fresh->height(), slices, fresh->mipmapLevelCount(),
                           uint32_t(t->key().base_page << 12));
      uint64_t offset = 0;
      for (uint32_t level = 0; level < fresh->mipmapLevelCount(); ++level) {
        const uint32_t w = std::max<uint32_t>(uint32_t(fresh->width()) >> level, 1);
        const uint32_t h = std::max<uint32_t>(uint32_t(fresh->height()) >> level, 1);
        const uint64_t row = uint64_t((w + bi.w - 1) / bi.w) * bi.bytes;
        const uint64_t image = row * ((h + bi.h - 1) / bi.h);
        for (uint32_t s = 0; s < slices; ++s) {
          blit->copyFromTexture(fresh, s, level, MTL::Origin::Make(0, 0, 0),
                                MTL::Size::Make(w, h, 1), p.a, offset, row,
                                image);
          blit->copyFromTexture(old, s, level, MTL::Origin::Make(0, 0, 0),
                                MTL::Size::Make(w, h, 1), p.b, offset, row,
                                image);
          offset += image;
        }
      }
      pairs->push_back(std::move(p));
    }
    if (blit) {
      blit->endEncoding();
      blit = nullptr;
    }
    if (!pairs->empty()) {
      cmd->addCompletedHandler([pairs](MTL::CommandBuffer*) {
        VerifyStats& st = Stats();
        for (Pair& p : *pairs) {
          const uint8_t* a = static_cast<const uint8_t*>(p.a->contents());
          const uint8_t* b = static_cast<const uint8_t*>(p.b->contents());
          const bool same = std::memcmp(a, b, size_t(p.length)) == 0;
          if (TexRenameVerifyBreak()) {
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1) < 64) {
              uint64_t ha = 1469598103934665603ull, nz = 0;
              for (uint64_t i = 0; i < p.length; ++i) {
                ha = (ha ^ a[i]) * 1099511628211ull;
                nz += a[i] != 0;
              }
              REXLOG_INFO("[tex-rename-verify] break sample {} hash {:016X} "
                          "nonzero {} of {}",
                          p.what, ha, nz, p.length);
            }
          }
          st.bytes.fetch_add(p.length, std::memory_order_relaxed);
          const uint64_t n_ok = same ? st.ok.fetch_add(1) + 1 : st.ok.load();
          const uint64_t n_bad =
              same ? st.mismatch.load() : st.mismatch.fetch_add(1) + 1;
          const uint64_t n = n_ok + n_bad;
          if (!same || (n & (n - 1)) == 0 || n % 1000 == 0) {
            std::string detail;
            if (!same) {
              uint64_t first = 0, diff = 0;
              for (uint64_t i = 0; i < p.length; ++i) {
                if (a[i] != b[i]) {
                  if (!diff) first = i;
                  ++diff;
                }
              }
              detail = fmt::format(" MISMATCH {} first +0x{:X} bytes {}",
                                   p.what, first, diff);
            }
            REXLOG_INFO(
                "[tex-rename-verify] ok {} mismatch {} skipped {} ({} KB "
                "compared){}",
                n_ok, n_bad, st.skipped.load(), st.bytes.load() >> 10, detail);
          }
          p.a->release();
          p.b->release();
        }
      });
    }
  }
  if (blit) blit->endEncoding();
  for (auto& [t, load] : renamed) {
    if (t->rename_src_) {
      t->rename_src_->release();
      t->rename_src_ = nullptr;
    }
  }
}

void MetalTextureCache::TexRenameSettleOrphans(MTL::CommandBuffer* cmd) {
  if (rename_pending_list_.empty()) return;
  bool any_pending = false;
  for (MetalTexture* t : rename_pending_list_) any_pending |= t->rename_pending_;
  MTL::BlitCommandEncoder* blit = nullptr;
  if (any_pending && cmd) {
    FlushPendingUploadEncodersForCommandEncoderBoundary();
  }
  for (MetalTexture* t : rename_pending_list_) {
    if (t->rename_pending_ && cmd) {
      ++UploadTrace().trn_orphan;
      TexRenameSettleOne(*t, cmd, blit);
    }
    if (t->rename_src_) {
      t->rename_src_->release();
      t->rename_src_ = nullptr;
    }
  }
  if (blit) blit->endEncoding();
  rename_pending_list_.clear();
}

void MetalTextureCache::TexRenameForget(MetalTexture* texture) {
  auto it = std::find(rename_pending_list_.begin(), rename_pending_list_.end(),
                      texture);
  if (it != rename_pending_list_.end()) rename_pending_list_.erase(it);
  if (texture->rename_src_) {
    texture->rename_src_->release();
    texture->rename_src_ = nullptr;
  }
}

bool MetalTextureCache::TexRenameAnyPending() const {
  for (const MetalTexture* t : rename_pending_list_) {
    if (t->rename_pending_) return true;
  }
  return false;
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
