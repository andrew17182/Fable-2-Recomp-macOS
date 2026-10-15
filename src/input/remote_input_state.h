// fable_2 - Remote input control: shared state
//
// InputStateStore is the single handoff between the remote control server
// thread(s) (host side) and the guest/main thread. The server publishes a
// resolved InputSnapshot; the RemoteGamepadDriver (an SDK InputDriver, polled
// by the guest input system) consumes the latest snapshot on each poll. The
// critical section is a copy of a tiny struct, so the guest-thread path cost
// is negligible. See plans/ai-remote-input-control.md.

#pragma once

#include <cstdint>
#include <mutex>

namespace fable2::remote {

// Resolved guest gamepad state: the X_INPUT_STATE fields the remote pad can
// drive (buttons / triggers / sticks).
struct InputSnapshot {
  uint16_t buttons = 0;      // X_INPUT_GAMEPAD_* button mask
  uint8_t left_trigger = 0;  // 0..255
  uint8_t right_trigger = 0; // 0..255
  int32_t stk_lx = 0;  // -32768..32767
  int32_t stk_ly = 0;  // positive = forward/up (Fable 2 convention)
  int32_t stk_rx = 0;
  int32_t stk_ry = 0;

  bool IsZero() const {
    return buttons == 0 && left_trigger == 0 && right_trigger == 0 &&
           stk_lx == 0 && stk_ly == 0 && stk_rx == 0 && stk_ry == 0;
  }
  bool operator==(const InputSnapshot&) const = default;
};

// Mutex-guarded snapshot store. Publish() returns false (no change) when the
// value is identical to the current one, so repeated resolutions of an
// unchanged timeline are free for the consumer.
class InputStateStore {
 public:
  // Returns true if the value actually changed.
  bool Publish(InputSnapshot s) {
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_ == s) return false;
    cur_ = s;
    return true;
  }

  InputSnapshot Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cur_;
  }

  // Optional observer, invoked (on the publishing thread) after a successful
  // publish. Used by the state probe to observe the A-press timestamp without
  // having to poll faster than the button is held. Pass nullptr to clear.
  void SetOnPublish(std::function<void(const InputSnapshot&)> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    on_publish_ = std::move(cb);
  }

 private:
  friend class ControlServer;
  bool PublishAndNotify(InputSnapshot s) {
    std::function<void(const InputSnapshot&)> cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (cur_ == s) return false;
      cur_ = s;
      cb = on_publish_;
    }
    if (cb) cb(s);
    return true;
  }

  mutable std::mutex mu_;
  InputSnapshot cur_{};
  std::function<void(const InputSnapshot&)> on_publish_;
};

}  // namespace fable2::remote
