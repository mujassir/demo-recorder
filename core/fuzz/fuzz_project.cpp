#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dr/project.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view text(reinterpret_cast<const char*>(data), size);
  auto doc = dr::json::parse(text);
  if (!doc) return 0;
  if (auto p = dr::project_from_json(*doc)) {
    for (const auto& s : p->sources) {
      if (!dr::is_safe_relative_path(s.file)) __builtin_trap();
    }
    if (!dr::project_from_json(dr::to_json(*p))) __builtin_trap();
  }
  return 0;
}
