#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dr/input_log.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view text(reinterpret_cast<const char*>(data), size);
  dr::parse_cursor_log(text);
  dr::parse_event_log(text);
  if (auto fixed = dr::recover_row_log_text(text)) {
    // Recovery output must be stable.
    auto twice = dr::recover_row_log_text(*fixed);
    if (!twice || *twice != *fixed) __builtin_trap();
  }
  return 0;
}
