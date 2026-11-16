// Master clock (spec: Architecture → Zero-copy recording pipeline, step 5).
//
// Every track (screen, camera, mic, system audio, cursor, key events) is
// stamped against one monotonic host clock:
//   macOS   mach_absolute_time   (same clock as CMClockGetHostTimeClock and
//                                 ScreenCaptureKit sample presentation times)
//   Windows QueryPerformanceCounter (same clock as
//                                 Direct3D11CaptureFrame.SystemRelativeTime and
//                                 WASAPI QPC positions)
//   Linux   CLOCK_MONOTONIC      (tests and tools only)
//
// Host timestamps are converted to nanoseconds once, at the edge, and
// everything inside the core works in int64 nanoseconds. Files store times
// relative to the recording epoch so they stay small and exact.
#pragma once

#include <cstdint>
#include <string_view>

namespace dr {

// ticks * numer / denom = nanoseconds
struct Timebase {
  uint64_t numer = 1;
  uint64_t denom = 1;
};

// Overflow-safe for any tick count whose result fits in int64 nanoseconds,
// provided numer and denom are below 2^32 (true for every real clock).
int64_t ticks_to_ns(uint64_t ticks, Timebase tb);
uint64_t ns_to_ticks(int64_t ns, Timebase tb);

class MasterClock {
 public:
  // Raw platform ticks; cheap and safe to call from real-time threads.
  static uint64_t now_ticks();
  static Timebase timebase();
  static int64_t now_ns();
  // Name stored in project.json → clock.domain.
  static std::string_view domain();
};

// Converts host nanoseconds into nanoseconds since the recording started.
// The epoch is taken when the user's countdown ends; samples captured before
// it (e.g. a frame already in flight) get negative times and are dropped by
// the editor.
class RecordingClock {
 public:
  explicit RecordingClock(int64_t epoch_host_ns) : epoch_(epoch_host_ns) {}
  int64_t epoch_host_ns() const { return epoch_; }
  int64_t to_recording_ns(int64_t host_ns) const { return host_ns - epoch_; }
  int64_t to_host_ns(int64_t recording_ns) const { return recording_ns + epoch_; }

 private:
  int64_t epoch_;
};

}  // namespace dr
