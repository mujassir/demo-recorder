#include "dr/clock.h"

#include <limits>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace dr {

int64_t ticks_to_ns(uint64_t ticks, Timebase tb) {
  if (tb.denom == 0) return 0;
  // Split to avoid overflowing ticks * numer.
  uint64_t whole = ticks / tb.denom;
  uint64_t rem = ticks % tb.denom;
  uint64_t ns = whole * tb.numer + (rem * tb.numer) / tb.denom;
  if (ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(ns);
}

uint64_t ns_to_ticks(int64_t ns, Timebase tb) {
  if (ns <= 0 || tb.numer == 0) return 0;
  auto u = static_cast<uint64_t>(ns);
  uint64_t whole = u / tb.numer;
  uint64_t rem = u % tb.numer;
  return whole * tb.denom + (rem * tb.denom) / tb.numer;
}

#if defined(__APPLE__)

uint64_t MasterClock::now_ticks() { return mach_absolute_time(); }

Timebase MasterClock::timebase() {
  static const Timebase tb = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return Timebase{info.numer, info.denom};
  }();
  return tb;
}

std::string_view MasterClock::domain() { return "mach_absolute_time"; }

#elif defined(_WIN32)

uint64_t MasterClock::now_ticks() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

Timebase MasterClock::timebase() {
  static const Timebase tb = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return Timebase{1'000'000'000ull, static_cast<uint64_t>(f.QuadPart)};
  }();
  return tb;
}

std::string_view MasterClock::domain() { return "qpc"; }

#else

uint64_t MasterClock::now_ticks() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

Timebase MasterClock::timebase() { return Timebase{1, 1}; }

std::string_view MasterClock::domain() { return "clock_monotonic"; }

#endif

int64_t MasterClock::now_ns() { return ticks_to_ns(now_ticks(), timebase()); }

}  // namespace dr
