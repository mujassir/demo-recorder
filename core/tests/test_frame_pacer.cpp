#include "dr/frame_pacer.h"
#include "test.h"

using namespace dr;

namespace {
// Feeds `seconds` of frames arriving at `source_hz` and counts accepted ones.
int accepted(uint32_t target_fps, double source_hz, double seconds, double jitter_ns = 0) {
  FramePacer pacer(target_fps);
  int kept = 0;
  int n = static_cast<int>(source_hz * seconds);
  for (int i = 0; i < n; ++i) {
    double t = i * 1e9 / source_hz + ((i % 2) ? jitter_ns : -jitter_ns);
    kept += pacer.accept(static_cast<int64_t>(t) + 1'000'000'000);
  }
  return kept;
}
}  // namespace

TEST(frame_pacer_caps_high_refresh_displays) {
  // 10 s of a 144 Hz display at a 60 fps target: ~600 frames, not ~720.
  int kept = accepted(60, 144, 10);
  CHECK(kept >= 590 && kept <= 601);
  kept = accepted(60, 120, 10);
  CHECK(kept >= 595 && kept <= 601);
  kept = accepted(30, 165, 10);
  CHECK(kept >= 295 && kept <= 301);
}

TEST(frame_pacer_keeps_every_frame_at_matching_rate) {
  CHECK(accepted(60, 60, 10) == 600);
  // Up to a quarter frame of vsync jitter still keeps every frame.
  CHECK(accepted(60, 60, 10, 2'000'000) == 600);
  CHECK(accepted(60, 30, 10) == 300);
}

TEST(frame_pacer_restarts_after_idle_gap) {
  FramePacer p(60);
  CHECK(p.accept(0));
  CHECK(!p.accept(5'000'000));
  // Screen static for 2 s, then a change: accepted, and the schedule restarts
  // from it instead of letting a burst of "overdue" frames through.
  CHECK(p.accept(2'000'000'000));
  CHECK(!p.accept(2'006'000'000));
  CHECK(p.accept(2'016'666'666));
}
