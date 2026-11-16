#include <cstring>
#include <filesystem>

#include "dr/dr_core.h"
#include "test.h"

TEST(c_api_project_lifecycle) {
  std::string dir = drtest::temp_dir("capi");
  int64_t epoch = dr_clock_now_ns();
  dr_project* p = dr_project_create("linux", "0.1.0", "test", epoch);
  REQUIRE(p);
  CHECK(std::strlen(dr_project_id(p)) == 36);
  CHECK(dr_project_add_display(p, "1", 1920, 1080, 1.0, 60.0) == DR_OK);
  CHECK(dr_project_add_video_source(p, "screen", DR_SOURCE_SCREEN, "screen.mp4", "1", "h264",
                                    1920, 1080, 60.0, 0) == DR_OK);
  CHECK(dr_project_add_audio_source(p, "mic", DR_SOURCE_MICROPHONE, "mic.wav", 48000, 1, 0) ==
        DR_OK);
  CHECK(dr_project_add_data_source(p, "cursor", DR_SOURCE_CURSOR, "cursor.json") == DR_OK);
  // Invalid input is reported, not crashed on.
  CHECK(dr_project_add_data_source(p, "cursor", DR_SOURCE_CURSOR, "cursor.json") ==
        DR_ERR_INVALID_ARGUMENT);
  CHECK(std::strstr(dr_last_error(), "duplicate") != nullptr);
  CHECK(dr_project_add_data_source(p, "x", DR_SOURCE_EVENTS, "../x.json") ==
        DR_ERR_INVALID_ARGUMENT);
  CHECK(dr_project_set_source_start(p, "screen", 16'000'000) == DR_OK);
  CHECK(dr_project_set_source_start(p, "nope", 1) == DR_ERR_INVALID_ARGUMENT);
  dr_project_set_duration_ns(p, 3'000'000'000);
  dr_project_mark_complete(p);
  CHECK(dr_project_save(p, dir.c_str()) == DR_OK);
  dr_project_free(p);

  dr_project* loaded = dr_project_load(dir.c_str());
  REQUIRE(loaded);
  CHECK(dr_project_duration_ns(loaded) == 3'000'000'000);
  CHECK(dr_project_source_count(loaded) == 3);
  dr_project_free(loaded);

  CHECK(dr_project_load("/definitely/not/here") == nullptr);
  CHECK(std::strlen(dr_last_error()) > 0);
  std::filesystem::remove_all(dir);
}

TEST(c_api_input_recorder_strips_key_identity) {
  std::string dir = drtest::temp_dir("capi_input");
  dr_input_recorder* r = dr_input_recorder_start(dir.c_str(), 0);
  REQUIRE(r);
  dr_input_recorder_push_cursor(r, 10, 1, 2, 0, 0);
  dr_input_recorder_push_event(r, 20, DR_EVENT_KEY_DOWN, 65);  // 'A' must not be stored
  CHECK(dr_input_recorder_stop(r) == DR_OK);
  std::FILE* f = std::fopen((dir + "/events.json").c_str(), "rb");
  REQUIRE(f);
  char buf[512] = {};
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  std::string text(buf, n);
  CHECK(text.find("[20,3,0]") != std::string::npos);
  CHECK(text.find("65") == std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(c_api_clock) {
  CHECK(dr_clock_now_ns() > 0);
  CHECK(std::strlen(dr_clock_domain()) > 0);
  CHECK(dr_project_format_version() == 0);
  CHECK(std::strcmp(dr_core_version(), "0.1.0") == 0);
}
