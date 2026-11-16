#include "dr/input_log.h"

#include <chrono>
#include <cstring>
#include <filesystem>

#include "dr/file_util.h"
#include "dr/json.h"

namespace dr {

namespace {

constexpr const char* kRowsKey = "rows";
constexpr std::string_view kFooter = "]}";
constexpr size_t kMaxLogBytes = size_t{1} << 30;  // 1 GiB; ~2h at 120 Hz is ~40 MB

const std::vector<std::string>& cursor_columns() {
  static const std::vector<std::string> c{"t_ns", "x", "y", "shape", "buttons"};
  return c;
}

const std::vector<std::string>& event_columns() {
  static const std::vector<std::string> c{"t_ns", "type", "code"};
  return c;
}

std::string make_header(const char* format_name, const std::vector<std::string>& columns) {
  std::string h = "{\"format\":";
  json::append_string(h, format_name);
  h += ",\"version\":";
  json::append_number(h, static_cast<int64_t>(kInputLogVersion));
  h += ",\"columns\":[";
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i) h.push_back(',');
    json::append_string(h, columns[i]);
  }
  h += "],\"";
  h += kRowsKey;
  h += "\":[";
  return h;
}

std::string_view trim_cr(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

// Splits text into lines; the last element has no trailing newline (it may
// be empty).
std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t start = 0;
  for (;;) {
    size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      lines.push_back(text.substr(start));
      return lines;
    }
    lines.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
}

// Strips the leading separator from a row line and checks it is a JSON array.
std::optional<json::Array> parse_row(std::string_view line, bool first) {
  line = trim_cr(line);
  if (!first) {
    if (line.empty() || line.front() != ',') return std::nullopt;
    line.remove_prefix(1);
  }
  if (line.empty() || line.front() != '[' || line.back() != ']') return std::nullopt;
  json::ParseOptions opts;
  opts.max_depth = 2;
  auto v = json::parse(line, nullptr, opts);
  if (!v || !v->is_array()) return std::nullopt;
  return std::move(*v->as_array());
}

struct HeaderCheck {
  bool ok = false;
  std::string error;
};

HeaderCheck check_header(std::string_view header_line, const char* format_name,
                         const std::vector<std::string>& columns) {
  HeaderCheck hc;
  std::string closed(trim_cr(header_line));
  closed += kFooter;
  auto doc = json::parse(closed);
  if (!doc || !doc->is_object()) {
    hc.error = "header is not valid JSON";
    return hc;
  }
  const json::Value* fmt = doc->find("format");
  if (!fmt || !fmt->as_string() || *fmt->as_string() != format_name) {
    hc.error = std::string("format is not ") + format_name;
    return hc;
  }
  const json::Value* ver = doc->find("version");
  auto v = ver ? ver->as_int() : std::nullopt;
  if (!v || *v < 0 || *v > kInputLogVersion) {
    hc.error = "unsupported version";
    return hc;
  }
  const json::Value* cols = doc->find("columns");
  const json::Array* ca = cols ? cols->as_array() : nullptr;
  if (!ca || ca->size() != columns.size()) {
    hc.error = "unexpected columns";
    return hc;
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string* s = (*ca)[i].as_string();
    if (!s || *s != columns[i]) {
      hc.error = "unexpected columns";
      return hc;
    }
  }
  const json::Value* rows = doc->find(kRowsKey);
  if (!rows || !rows->is_array() || !rows->as_array()->empty()) {
    hc.error = "header must end with an open rows array";
    return hc;
  }
  hc.ok = true;
  return hc;
}

// Generic line-oriented reader. `on_row` returns false to reject a row.
template <typename OnRow>
bool read_row_log(std::string_view text, const char* format_name,
                  const std::vector<std::string>& columns, std::string* error, OnRow on_row) {
  auto set = [&](std::string msg) {
    if (error) *error = std::move(msg);
    return false;
  };
  if (text.size() > kMaxLogBytes) return set("file too large");
  auto lines = split_lines(text);
  if (lines.empty()) return set("empty file");
  HeaderCheck hc = check_header(lines[0], format_name, columns);
  if (!hc.ok) return set(hc.error);
  bool closed = false;
  size_t row_index = 0;
  for (size_t i = 1; i < lines.size(); ++i) {
    std::string_view line = trim_cr(lines[i]);
    if (closed) {
      if (!line.empty()) return set("data after end of log");
      continue;
    }
    if (line == kFooter) {
      closed = true;
      continue;
    }
    auto row = parse_row(line, row_index == 0);
    if (!row) return set("line " + std::to_string(i + 1) + ": malformed row");
    if (row->size() != columns.size()) {
      return set("line " + std::to_string(i + 1) + ": wrong number of columns");
    }
    if (!on_row(*row)) return set("line " + std::to_string(i + 1) + ": invalid value");
    ++row_index;
  }
  if (!closed) return set("log is not closed (run recovery)");
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoding

std::string encode_row(const CursorSample& s) {
  std::string row = "[";
  json::append_number(row, s.t_ns);
  row.push_back(',');
  json::append_number(row, s.x);
  row.push_back(',');
  json::append_number(row, s.y);
  row.push_back(',');
  json::append_number(row, static_cast<int64_t>(s.shape_id));
  row.push_back(',');
  json::append_number(row, static_cast<int64_t>(s.buttons));
  row.push_back(']');
  return row;
}

std::string encode_row(const InputEvent& e) {
  std::string row = "[";
  json::append_number(row, e.t_ns);
  row.push_back(',');
  json::append_number(row, static_cast<int64_t>(e.type));
  row.push_back(',');
  json::append_number(row, static_cast<int64_t>(e.code));
  row.push_back(']');
  return row;
}

// ---------------------------------------------------------------------------
// RowLogWriter

RowLogWriter::~RowLogWriter() { close(); }

bool RowLogWriter::open(const std::string& path, const char* format_name,
                        const std::vector<std::string>& columns, std::string* error) {
  close();
#if defined(_WIN32)
  // Paths are UTF-8; the wide CRT API needs UTF-16.
  std::filesystem::path wpath(std::u8string(path.begin(), path.end()));
  file_ = _wfopen(wpath.c_str(), L"wb");
#else
  file_ = std::fopen(path.c_str(), "wb");
#endif
  if (!file_) {
    if (error) *error = path + ": cannot open for writing";
    return false;
  }
  rows_ = 0;
  std::string header = make_header(format_name, columns);
  header.push_back('\n');
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    if (error) *error = path + ": write failed";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  return flush();
}

bool RowLogWriter::append_row(const std::string& row_json) {
  if (!file_) return false;
  if (rows_ > 0 && std::fputc(',', file_) == EOF) return false;
  if (std::fwrite(row_json.data(), 1, row_json.size(), file_) != row_json.size()) return false;
  if (std::fputc('\n', file_) == EOF) return false;
  ++rows_;
  return true;
}

bool RowLogWriter::flush() { return file_ && std::fflush(file_) == 0; }

bool RowLogWriter::close() {
  if (!file_) return true;
  bool ok = std::fwrite(kFooter.data(), 1, kFooter.size(), file_) == kFooter.size();
  ok = std::fputc('\n', file_) != EOF && ok;
  ok = std::fclose(file_) == 0 && ok;
  file_ = nullptr;
  return ok;
}

// ---------------------------------------------------------------------------
// InputRecorder

InputRecorder::InputRecorder(RecordingClock clock, size_t ring_capacity)
    : clock_(clock), cursor_ring_(ring_capacity), event_ring_(ring_capacity) {}

InputRecorder::~InputRecorder() { stop(); }

bool InputRecorder::start(const std::string& dir, std::string* error) {
  if (running_.load()) return true;
  if (!cursor_writer_.open(join_path(dir, kCursorFileName), kCursorFormatName, cursor_columns(),
                           error)) {
    return false;
  }
  if (!event_writer_.open(join_path(dir, kEventsFileName), kEventsFormatName, event_columns(),
                          error)) {
    cursor_writer_.close();
    return false;
  }
  running_.store(true);
  thread_ = std::thread([this] { drain_loop(); });
  return true;
}

void InputRecorder::push_cursor(int64_t host_ns, double x, double y, int32_t shape_id,
                                uint32_t buttons) {
  CursorSample s{clock_.to_recording_ns(host_ns), x, y, shape_id, buttons};
  if (!cursor_ring_.try_push(s)) cursor_dropped_.fetch_add(1, std::memory_order_relaxed);
}

void InputRecorder::push_event(int64_t host_ns, InputEventType type, int32_t code) {
  InputEvent e{clock_.to_recording_ns(host_ns), type, code};
  if (!event_ring_.try_push(e)) event_dropped_.fetch_add(1, std::memory_order_relaxed);
}

void InputRecorder::drain_once() {
  CursorSample s;
  while (cursor_ring_.try_pop(s)) {
    if (!cursor_writer_.append_row(encode_row(s))) write_failed_ = true;
    cursor_rows_.fetch_add(1, std::memory_order_relaxed);
  }
  InputEvent e;
  while (event_ring_.try_pop(e)) {
    if (!event_writer_.append_row(encode_row(e))) write_failed_ = true;
    event_rows_.fetch_add(1, std::memory_order_relaxed);
  }
}

void InputRecorder::drain_loop() {
  using namespace std::chrono;
  auto last_flush = steady_clock::now();
  while (running_.load(std::memory_order_acquire)) {
    drain_once();
    auto now = steady_clock::now();
    if (now - last_flush >= seconds(1)) {
      cursor_writer_.flush();
      event_writer_.flush();
      last_flush = now;
    }
    std::this_thread::sleep_for(milliseconds(20));
  }
}

bool InputRecorder::stop() {
  if (!thread_.joinable()) return !write_failed_;
  running_.store(false, std::memory_order_release);
  thread_.join();
  drain_once();
  bool ok = cursor_writer_.close();
  ok = event_writer_.close() && ok;
  return ok && !write_failed_;
}

InputRecorderStats InputRecorder::stats() const {
  InputRecorderStats st;
  st.cursor_rows = cursor_rows_.load();
  st.event_rows = event_rows_.load();
  st.cursor_dropped = cursor_dropped_.load();
  st.event_dropped = event_dropped_.load();
  return st;
}

// ---------------------------------------------------------------------------
// Readers

std::optional<CursorLog> parse_cursor_log(std::string_view text, std::string* error) {
  CursorLog log;
  bool ok = read_row_log(text, kCursorFormatName, cursor_columns(), error,
                         [&](const json::Array& row) {
                           auto t = row[0].as_int();
                           auto x = row[1].as_double();
                           auto y = row[2].as_double();
                           auto shape = row[3].as_int();
                           auto buttons = row[4].as_int();
                           if (!t || !x || !y || !shape || !buttons) return false;
                           if (*shape < 0 || *shape > INT32_MAX) return false;
                           if (*buttons < 0 || *buttons > UINT32_MAX) return false;
                           log.samples.push_back(CursorSample{*t, *x, *y, static_cast<int32_t>(*shape),
                                                              static_cast<uint32_t>(*buttons)});
                           return true;
                         });
  if (!ok) return std::nullopt;
  return log;
}

std::optional<EventLog> parse_event_log(std::string_view text, std::string* error) {
  EventLog log;
  bool ok = read_row_log(text, kEventsFormatName, event_columns(), error,
                         [&](const json::Array& row) {
                           auto t = row[0].as_int();
                           auto type = row[1].as_int();
                           auto code = row[2].as_int();
                           if (!t || !type || !code) return false;
                           if (*type < 1 || *type > 4) return false;
                           if (*code < 0 || *code > 255) return false;
                           log.events.push_back(InputEvent{*t, static_cast<InputEventType>(*type),
                                                           static_cast<int32_t>(*code)});
                           return true;
                         });
  if (!ok) return std::nullopt;
  return log;
}

// ---------------------------------------------------------------------------
// Recovery

std::optional<std::string> recover_row_log_text(std::string_view text, RecoveryResult* result) {
  auto lines = split_lines(text);
  if (lines.empty()) return std::nullopt;
  std::string_view header = trim_cr(lines[0]);
  std::string open_rows = std::string("\"") + kRowsKey + "\":[";
  if (header.size() < open_rows.size() || header.front() != '{' ||
      header.substr(header.size() - open_rows.size()) != open_rows) {
    return std::nullopt;
  }
  // The header line must have been written completely (it ends in a newline).
  if (lines.size() < 2) return std::nullopt;
  {
    std::string closed(header);
    closed += kFooter;
    if (!json::parse(closed)) return std::nullopt;
  }

  std::string out(header);
  out.push_back('\n');
  uint64_t kept = 0;
  for (size_t i = 1; i < lines.size(); ++i) {
    std::string_view line = trim_cr(lines[i]);
    if (line == kFooter) break;
    if (!parse_row(line, kept == 0)) break;  // partial or garbage: stop here
    out.append(line);
    out.push_back('\n');
    ++kept;
  }
  out.append(kFooter);
  out.push_back('\n');

  if (result) {
    result->rows_kept = kept;
    result->changed = out != text;
    result->bytes_dropped = text.size() > out.size() ? text.size() - out.size() : 0;
  }
  return out;
}

std::optional<RecoveryResult> recover_row_log(const std::string& path, std::string* error) {
  auto text = read_file(path, kMaxLogBytes, error);
  if (!text) return std::nullopt;
  RecoveryResult result;
  auto fixed = recover_row_log_text(*text, &result);
  if (!fixed) {
    if (error) *error = path + ": header is damaged; cannot recover";
    return std::nullopt;
  }
  if (result.changed && !write_file_atomic(path, *fixed, error)) return std::nullopt;
  return result;
}

}  // namespace dr
