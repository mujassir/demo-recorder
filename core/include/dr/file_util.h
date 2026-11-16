#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dr {

std::string join_path(std::string_view dir, std::string_view name);

// Reads a whole file. Fails if the file is larger than max_bytes.
std::optional<std::string> read_file(const std::string& path, size_t max_bytes,
                                     std::string* error = nullptr);

// Writes via "<path>.tmp" then renames over <path>.
bool write_file_atomic(const std::string& path, std::string_view contents,
                       std::string* error = nullptr);

}  // namespace dr
