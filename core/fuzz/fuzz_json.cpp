#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dr/json.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view text(reinterpret_cast<const char*>(data), size);
  if (auto v = dr::json::parse(text)) {
    // Anything we accept must serialize and re-parse to the same value.
    auto again = dr::json::parse(dr::json::serialize(*v));
    if (!again || *again != *v) __builtin_trap();
  }
  return 0;
}
