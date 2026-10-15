#pragma once
// [pass-merge] Fewer render-pass breaks with the same GPU result (docs/pass-merge-20261013.md).
// FABLE2_PASS_MERGE=0 turns every item off (master). Items, each default on, =0 off:
// - FABLE2_PASS_MERGE_TEX_DIRECT: a texture load routed to the pre-commit command buffer that
//   finds its guest range not yet uploaded no longer abandons the pre-commit route when every
//   invalid page can take the CPU direct write (no reader of the page in flight or in the open
//   command buffer). The upload is then a memcpy with no GPU command, so the render pass stays
//   open. Anything else (a staged copy) still ends the pass as before.
// - FABLE2_PASS_MERGE_CLEAR_DST: a deferred resolve clear of a render target that receives
//   ownership transfers in the next draw pass is drawn inside that pass, first, instead of as a
//   separate clear pass (same writes in the same order).
#include <cstdlib>

#include "rex/logging.h"

namespace rex::graphics::metal {

inline bool PassMergeEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PASS_MERGE");
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[pass-merge] {} (FABLE2_PASS_MERGE)", e ? "on" : "off");
    return e;
  }();
  return on;
}

inline bool PassMergeItem(const char* name) {
  const char* v = std::getenv(name);  // getenv-ok: called from static initializers only
  const bool e = PassMergeEnabled() && !(v && v[0] == '0');
  REXLOG_INFO("[pass-merge] {} {}", name, e ? "on" : "off");
  return e;
}

inline bool PassMergeTexDirectEnabled() {
  static const bool on = PassMergeItem("FABLE2_PASS_MERGE_TEX_DIRECT");  // getenv-ok: static init
  return on;
}

inline bool PassMergeClearDstEnabled() {
  static const bool on = PassMergeItem("FABLE2_PASS_MERGE_CLEAR_DST");  // getenv-ok: static init
  return on;
}

}  // namespace rex::graphics::metal
