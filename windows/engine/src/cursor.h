// 120 Hz cursor sampler (spec I-1). P0 stand-in: P1 adds low-level mouse and
// keyboard hooks (WH_MOUSE_LL / WH_KEYBOARD_LL) for exact click and key
// timing, and cursor shape capture (I-4).
#pragma once

#include <windows.h>

#include <atomic>
#include <thread>

#include "dr/input_log.h"

namespace drwin {

class CursorSampler {
 public:
  // `monitor` is in physical pixels (the process is per-monitor DPI aware).
  CursorSampler(dr::InputRecorder& recorder, RECT monitor, uint32_t output_width, uint32_t output_height);
  ~CursorSampler();

  void start();
  void stop();

 private:
  void run();

  dr::InputRecorder& recorder_;
  RECT monitor_;
  double scale_x_;
  double scale_y_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace drwin
