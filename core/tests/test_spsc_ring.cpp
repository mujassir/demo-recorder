#include <thread>

#include "dr/spsc_ring.h"
#include "test.h"

using namespace dr;

TEST(ring_capacity_rounds_up) {
  SpscRing<int> r(5);
  CHECK(r.capacity() == 8);
}

TEST(ring_full_and_empty) {
  SpscRing<int> r(4);
  for (int i = 0; i < 4; ++i) CHECK(r.try_push(i));
  CHECK(!r.try_push(99));
  int v = -1;
  for (int i = 0; i < 4; ++i) {
    CHECK(r.try_pop(v));
    CHECK(v == i);
  }
  CHECK(!r.try_pop(v));
}

TEST(ring_two_threads_preserve_order) {
  SpscRing<uint64_t> r(1024);
  constexpr uint64_t kCount = 2'000'000;
  std::thread producer([&] {
    for (uint64_t i = 0; i < kCount;) {
      if (r.try_push(i)) ++i;
    }
  });
  uint64_t expected = 0;
  bool in_order = true;
  while (expected < kCount) {
    uint64_t v;
    if (r.try_pop(v)) {
      in_order = in_order && v == expected;
      ++expected;
    }
  }
  producer.join();
  CHECK(in_order);
}
