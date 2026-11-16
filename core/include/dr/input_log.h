// Cursor and input logs (spec I-1, I-2): cursor.json and events.json.
//
// Both files are "row logs": a JSON object whose last member is an array of
// fixed-width tuple rows, one row per line. They are written incrementally
// while recording, so a crash leaves a file that is complete up to the last
// flushed row; `recover_row_log` closes such a file so it parses again.
//
//   {"format":"demo-recorder-cursor","version":0,"columns":[...],"rows":[
//   [0,512.5,384.0,0,0]
//   ,[8333333,513.0,384.5,0,0]
//   ]}
//
// Times are int64 nanoseconds since the recording epoch.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dr/clock.h"
#include "dr/spsc_ring.h"

namespace dr {

inline constexpr const char* kCursorFileName = "cursor.json";
inline constexpr const char* kEventsFileName = "events.json";
inline constexpr const char* kCursorFormatName = "demo-recorder-cursor";
inline constexpr const char* kEventsFormatName = "demo-recorder-events";
inline constexpr int kInputLogVersion = 0;

// Mouse button bit flags in CursorSample::buttons.
enum MouseButtons : uint32_t {
  kButtonLeft = 1u << 0,
  kButtonRight = 1u << 1,
  kButtonMiddle = 1u << 2,
};

struct CursorSample {
  int64_t t_ns = 0;       // recording time
  double x = 0;           // capture-space pixels, origin top-left of the captured area
  double y = 0;
  int32_t shape_id = 0;   // index into cursors/ (0 = unknown / system arrow)
  uint32_t buttons = 0;   // MouseButtons
};

enum class InputEventType : int32_t {
  MouseDown = 1,
  MouseUp = 2,
  KeyDown = 3,
  KeyUp = 4,
};

struct InputEvent {
  int64_t t_ns = 0;
  InputEventType type = InputEventType::MouseDown;
  // Mouse: button index (0 left, 1 right, 2 middle, 3+ other).
  // Key: always 0 — key identity is never stored in v0 (privacy rule; the
  // keystroke overlay data of I-3 will live in a separate opt-in file).
  int32_t code = 0;
};

// ---------------------------------------------------------------------------
// Low-level streaming writer for one row log.

class RowLogWriter {
 public:
  RowLogWriter() = default;
  ~RowLogWriter();
  RowLogWriter(const RowLogWriter&) = delete;
  RowLogWriter& operator=(const RowLogWriter&) = delete;

  bool open(const std::string& path, const char* format_name, const std::vector<std::string>& columns,
            std::string* error = nullptr);
  // `row_json` is one encoded tuple, e.g. "[0,1.5,2]".
  bool append_row(const std::string& row_json);
  bool flush();
  bool close();
  bool is_open() const { return file_ != nullptr; }
  uint64_t rows() const { return rows_; }

 private:
  std::FILE* file_ = nullptr;
  uint64_t rows_ = 0;
};

// Encodes rows (shared by the writer and tests).
std::string encode_row(const CursorSample& s);
std::string encode_row(const InputEvent& e);

// ---------------------------------------------------------------------------
// Recorder: real-time producers push into lock-free rings; a background
// thread drains them to disk and flushes about once per second.
//
// Threading: push_cursor must only be called from one thread at a time, and
// push_event from one thread at a time (they may be different threads).
// Neither allocates, locks or blocks.

struct InputRecorderStats {
  uint64_t cursor_rows = 0;
  uint64_t event_rows = 0;
  uint64_t cursor_dropped = 0;  // ring full; should stay 0
  uint64_t event_dropped = 0;
};

class InputRecorder {
 public:
  InputRecorder(RecordingClock clock, size_t ring_capacity = 8192);
  ~InputRecorder();
  InputRecorder(const InputRecorder&) = delete;
  InputRecorder& operator=(const InputRecorder&) = delete;

  // Creates cursor.json and events.json in `dir` and starts the drain thread.
  bool start(const std::string& dir, std::string* error = nullptr);

  void push_cursor(int64_t host_ns, double x, double y, int32_t shape_id, uint32_t buttons);
  void push_event(int64_t host_ns, InputEventType type, int32_t code);

  // Drains everything, closes both files and joins the thread.
  bool stop();

  InputRecorderStats stats() const;

 private:
  void drain_loop();
  void drain_once();

  RecordingClock clock_;
  SpscRing<CursorSample> cursor_ring_;
  SpscRing<InputEvent> event_ring_;
  RowLogWriter cursor_writer_;
  RowLogWriter event_writer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> cursor_dropped_{0};
  std::atomic<uint64_t> event_dropped_{0};
  std::atomic<uint64_t> cursor_rows_{0};
  std::atomic<uint64_t> event_rows_{0};
  bool write_failed_ = false;
};

// ---------------------------------------------------------------------------
// Readers and crash recovery.

struct CursorLog {
  std::vector<CursorSample> samples;
};

struct EventLog {
  std::vector<InputEvent> events;
};

std::optional<CursorLog> parse_cursor_log(std::string_view text, std::string* error = nullptr);
std::optional<EventLog> parse_event_log(std::string_view text, std::string* error = nullptr);

struct RecoveryResult {
  bool changed = false;     // false when the file was already complete
  uint64_t rows_kept = 0;
  uint64_t bytes_dropped = 0;
};

// Repairs a row log truncated by a crash: keeps the header and every complete
// row, drops a partial trailing row, and writes the closing "]}". Idempotent.
std::optional<RecoveryResult> recover_row_log(const std::string& path, std::string* error = nullptr);

// Same repair on an in-memory buffer. Returns nullopt if the header is damaged.
std::optional<std::string> recover_row_log_text(std::string_view text, RecoveryResult* result = nullptr);

}  // namespace dr
