// Caps a variable-rate frame stream (capture APIs deliver at the display's
// refresh rate, only when content changes) to a target frame rate.
//
// Frames are accepted on a fixed schedule rather than by distance from the
// last accepted frame; the latter lets every other frame of a 144 Hz display
// through (~72 fps for a 60 fps target). A tolerance of a quarter frame
// absorbs vsync jitter, and after an idle gap the schedule restarts.
#pragma once

#include <cstdint>

namespace dr {

class FramePacer {
 public:
  explicit FramePacer(uint32_t fps)
      : interval_ns_(1'000'000'000LL / (fps ? fps : 1)), tolerance_ns_(interval_ns_ / 4) {}

  // Returns true if the frame stamped `t_ns` should be kept. Not thread-safe;
  // call from the capture callback thread only.
  bool accept(int64_t t_ns) {
    if (next_due_ns_ >= 0 && t_ns + tolerance_ns_ < next_due_ns_) return false;
    next_due_ns_ = (next_due_ns_ < 0 || t_ns - next_due_ns_ >= interval_ns_)
                       ? t_ns + interval_ns_
                       : next_due_ns_ + interval_ns_;
    return true;
  }

  int64_t interval_ns() const { return interval_ns_; }

 private:
  int64_t interval_ns_;
  int64_t tolerance_ns_;
  int64_t next_due_ns_ = -1;
};

}  // namespace dr
