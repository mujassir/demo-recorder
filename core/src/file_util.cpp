#include "dr/file_util.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dr {

namespace fs = std::filesystem;

namespace {
fs::path to_path(const std::string& utf8) {
#if defined(__cpp_char8_t)
  return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
  return fs::u8path(utf8);
#endif
}
}  // namespace

std::string join_path(std::string_view dir, std::string_view name) {
  std::string out(dir);
  if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
  out.append(name);
  return out;
}

std::optional<std::string> read_file(const std::string& path, size_t max_bytes,
                                     std::string* error) {
  std::error_code ec;
  fs::path p = to_path(path);
  auto size = fs::file_size(p, ec);
  if (ec) {
    if (error) *error = path + ": " + ec.message();
    return std::nullopt;
  }
  if (size > max_bytes) {
    if (error) *error = path + ": file too large";
    return std::nullopt;
  }
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (error) *error = path + ": cannot open";
    return std::nullopt;
  }
  std::string data(static_cast<size_t>(size), '\0');
  in.read(data.data(), static_cast<std::streamsize>(size));
  if (static_cast<size_t>(in.gcount()) != size) {
    if (error) *error = path + ": short read";
    return std::nullopt;
  }
  return data;
}

namespace {

std::FILE* open_for_write(const fs::path& p) {
#if defined(_WIN32)
  return _wfopen(p.c_str(), L"wb");
#else
  return std::fopen(p.c_str(), "wb");
#endif
}

// Forces file data to stable storage, so a rename that survives a power loss
// never points at an empty or partial file.
bool sync_file(std::FILE* f) {
#if defined(_WIN32)
  return _commit(_fileno(f)) == 0;
#elif defined(__APPLE__)
  // fsync on macOS only reaches the drive cache; F_FULLFSYNC flushes it.
  return fcntl(fileno(f), F_FULLFSYNC) == 0 || fsync(fileno(f)) == 0;
#else
  return fsync(fileno(f)) == 0;
#endif
}

}  // namespace

bool write_file_atomic(const std::string& path, std::string_view contents, std::string* error) {
  fs::path target = to_path(path);
  fs::path tmp = target;
  tmp += ".tmp";
  std::FILE* f = open_for_write(tmp);
  if (!f) {
    if (error) *error = path + ": cannot open for writing";
    return false;
  }
  bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
  ok = std::fflush(f) == 0 && ok;
  ok = ok && sync_file(f);
  ok = std::fclose(f) == 0 && ok;
  std::error_code ec;
  if (!ok) {
    if (error) *error = path + ": write failed";
    fs::remove(tmp, ec);
    return false;
  }
  // fs::rename replaces an existing target on POSIX and Windows.
  fs::rename(tmp, target, ec);
  if (ec) {
    if (error) *error = path + ": rename failed: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

}  // namespace dr
