#include <thread>

#include "dr/clock.h"
#include "test.h"

using namespace dr;

TEST(clock_ticks_to_ns_apple_silicon_timebase) {
  Timebase tb{125, 3};  // M1/M2/M3: 24 MHz
  CHECK(ticks_to_ns(24'000'000, tb) == 1'000'000'000);
  CHECK(ticks_to_ns(1, tb) == 41);
  // ~300 days of uptime must not overflow the intermediate product.
  uint64_t ticks = 24'000'000ull * 86'400ull * 300ull;
  CHECK(ticks_to_ns(ticks, tb) == 1'000'000'000ll * 86'400ll * 300ll);
}

TEST(clock_ticks_to_ns_qpc_timebase) {
  Timebase tb{1'000'000'000ull, 10'000'000ull};  // 10 MHz QPC
  CHECK(ticks_to_ns(10'000'000, tb) == 1'000'000'000);
  CHECK(ticks_to_ns(3, tb) == 300);
  CHECK(ns_to_ticks(1'000'000'000, tb) == 10'000'000);
}

TEST(clock_round_trip) {
  Timebase tb{125, 3};
  for (uint64_t t : {0ull, 3ull, 999'999ull, 123'456'789'012ull}) {
    int64_t ns = ticks_to_ns(t, tb);
    uint64_t back = ns_to_ticks(ns, tb);
    CHECK(back <= t && t - back <= 1);
  }
}

TEST(clock_is_monotonic) {
  int64_t a = MasterClock::now_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  int64_t b = MasterClock::now_ns();
  CHECK(b > a);
  CHECK(b - a >= 4'000'000);
  CHECK(!MasterClock::domain().empty());
}

TEST(recording_clock_offsets) {
  RecordingClock rc(1'000'000);
  CHECK(rc.to_recording_ns(1'500'000) == 500'000);
  CHECK(rc.to_recording_ns(900'000) == -100'000);
  CHECK(rc.to_host_ns(500'000) == 1'500'000);
}
