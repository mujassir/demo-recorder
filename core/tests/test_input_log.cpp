#include <filesystem>
#include <thread>

#include "dr/file_util.h"
#include "dr/input_log.h"
#include "test.h"

using namespace dr;

namespace {
std::string read_all(const std::string& path) { return *read_file(path, 1 << 30); }
}  // namespace

TEST(input_recorder_writes_parseable_logs) {
  std::string dir = drtest::temp_dir("input");
  const int64_t epoch = 5'000'000'000;
  InputRecorder rec{RecordingClock(epoch)};
  REQUIRE(rec.start(dir));
  for (int i = 0; i < 1000; ++i) {
    rec.push_cursor(epoch + i * 8'333'333LL, 100.5 + i, 200.25, i % 3, i % 2 ? kButtonLeft : 0);
  }
  rec.push_event(epoch + 10, InputEventType::MouseDown, 0);
  rec.push_event(epoch + 20, InputEventType::MouseUp, 0);
  rec.push_event(epoch + 30, InputEventType::KeyDown, 0);
  REQUIRE(rec.stop());
  auto st = rec.stats();
  CHECK(st.cursor_rows == 1000);
  CHECK(st.event_rows == 3);
  CHECK(st.cursor_dropped == 0);

  std::string err;
  auto cursor = parse_cursor_log(read_all(join_path(dir, kCursorFileName)), &err);
  if (!cursor) std::fprintf(stderr, "    %s\n", err.c_str());
  REQUIRE(cursor);
  REQUIRE(cursor->samples.size() == 1000);
  CHECK(cursor->samples[0].t_ns == 0);
  CHECK(cursor->samples[999].t_ns == 999 * 8'333'333LL);
  CHECK(cursor->samples[1].x == 101.5);
  CHECK(cursor->samples[1].y == 200.25);
  CHECK(cursor->samples[1].buttons == kButtonLeft);
  CHECK(cursor->samples[2].shape_id == 2);

  auto events = parse_event_log(read_all(join_path(dir, kEventsFileName)), &err);
  REQUIRE(events);
  REQUIRE(events->events.size() == 3);
  CHECK(events->events[1].type == InputEventType::MouseUp);
  CHECK(events->events[2].t_ns == 30);
  std::filesystem::remove_all(dir);
}

TEST(input_recorder_concurrent_producers) {
  std::string dir = drtest::temp_dir("input_mt");
  InputRecorder rec{RecordingClock(0), 1 << 16};
  REQUIRE(rec.start(dir));
  std::thread cursor_thread([&] {
    for (int i = 0; i < 20000; ++i) rec.push_cursor(i, i, i, 0, 0);
  });
  std::thread event_thread([&] {
    for (int i = 0; i < 5000; ++i) rec.push_event(i, InputEventType::MouseDown, 1);
  });
  cursor_thread.join();
  event_thread.join();
  REQUIRE(rec.stop());
  auto st = rec.stats();
  CHECK(st.cursor_rows + st.cursor_dropped == 20000);
  CHECK(st.event_rows + st.event_dropped == 5000);
  auto cursor = parse_cursor_log(read_all(join_path(dir, kCursorFileName)));
  REQUIRE(cursor);
  CHECK(cursor->samples.size() == st.cursor_rows);
  std::filesystem::remove_all(dir);
}

TEST(row_log_recovers_truncated_file) {
  std::string dir = drtest::temp_dir("recover");
  std::string path = join_path(dir, kCursorFileName);
  {
    RowLogWriter w;
    REQUIRE(w.open(path, kCursorFormatName, {"t_ns", "x", "y", "shape", "buttons"}));
    for (int i = 0; i < 10; ++i) {
      REQUIRE(w.append_row(encode_row(CursorSample{i, 1.0, 2.0, 0, 0})));
    }
    REQUIRE(w.close());
  }
  std::string full = read_all(path);
  // Simulate a crash: drop the footer and cut the last row in half.
  std::string crashed = full.substr(0, full.size() - 3);  // remove "]}\n"
  crashed = crashed.substr(0, crashed.size() - 6);
  REQUIRE(write_file_atomic(path, crashed));
  CHECK(!parse_cursor_log(read_all(path)));

  std::string err;
  auto r = recover_row_log(path, &err);
  REQUIRE(r);
  CHECK(r->changed);
  CHECK(r->rows_kept == 9);
  auto log = parse_cursor_log(read_all(path), &err);
  REQUIRE(log);
  CHECK(log->samples.size() == 9);
  CHECK(log->samples.back().t_ns == 8);

  // Idempotent on a complete file.
  auto again = recover_row_log(path, &err);
  REQUIRE(again);
  CHECK(!again->changed);
  CHECK(again->rows_kept == 9);
  std::filesystem::remove_all(dir);
}

TEST(row_log_recovers_header_only_file) {
  std::string header =
      "{\"format\":\"demo-recorder-events\",\"version\":0,\"columns\":[\"t_ns\",\"type\",\"code\"],"
      "\"rows\":[\n";
  RecoveryResult r;
  auto fixed = recover_row_log_text(header, &r);
  REQUIRE(fixed);
  CHECK(r.rows_kept == 0);
  auto log = parse_event_log(*fixed);
  REQUIRE(log);
  CHECK(log->events.empty());
  // A header cut mid-line cannot be trusted.
  CHECK(!recover_row_log_text(header.substr(0, 30)));
}

TEST(row_log_reader_rejects_bad_rows) {
  std::string head =
      "{\"format\":\"demo-recorder-events\",\"version\":0,\"columns\":[\"t_ns\",\"type\",\"code\"],"
      "\"rows\":[\n";
  CHECK(parse_event_log(head + "[1,1,0]\n,[2,2,0]\n]}\n").has_value());
  CHECK(!parse_event_log(head + "[1,9,0]\n]}\n"));        // unknown type
  CHECK(!parse_event_log(head + "[1,1]\n]}\n"));          // wrong arity
  CHECK(!parse_event_log(head + "[1,1,0]\n[2,1,0]\n]}\n"));  // missing comma
  CHECK(!parse_event_log(head + "[1,1,0]\n]}\ngarbage\n"));
  CHECK(!parse_cursor_log(head + "]}\n"));                // wrong format name
}
