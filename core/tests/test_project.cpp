#include <filesystem>

#include "dr/file_util.h"
#include "dr/project.h"
#include "test.h"

using namespace dr;

namespace {

Project sample_project() {
  Project p = make_project(AppInfo{"Demo Recorder", "0.1.0", "windows", "10.0.22631"},
                           ClockInfo{"qpc", 123'456'789'000});
  p.displays.push_back(Display{"\\\\.\\DISPLAY1", 2560, 1440, 1.5, 144.0});
  Source screen;
  screen.id = "screen";
  screen.kind = SourceKind::Screen;
  screen.file = "screen.mp4";
  screen.display_id = "\\\\.\\DISPLAY1";
  screen.start_offset_ns = 1'234;
  screen.video = VideoInfo{"h264", 2560, 1440, 60.0};
  p.sources.push_back(screen);
  Source mic;
  mic.id = "mic";
  mic.kind = SourceKind::Microphone;
  mic.file = "mic.wav";
  mic.audio = AudioInfo{48000, 2};
  p.sources.push_back(mic);
  p.duration_ns = 5'000'000'000;
  return p;
}

std::optional<Project> reparse(const json::Value& doc, std::string* msg = nullptr) {
  LoadError err;
  auto p = project_from_json(doc, &err);
  if (msg) *msg = err.message;
  return p;
}

}  // namespace

TEST(project_uuid_format) {
  std::string a = generate_uuid_v4();
  std::string b = generate_uuid_v4();
  CHECK(a.size() == 36);
  CHECK(a != b);
  CHECK(a[14] == '4');
  CHECK(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b');
}

TEST(project_round_trip_through_json) {
  Project p = sample_project();
  auto text = json::serialize(to_json(p));
  auto doc = json::parse(text);
  REQUIRE(doc);
  auto back = reparse(*doc);
  REQUIRE(back);
  CHECK(back->id == p.id);
  CHECK(back->clock.epoch_host_ns == 123'456'789'000);
  CHECK(back->displays.size() == 1);
  CHECK(back->displays[0].scale == 1.5);
  REQUIRE(back->sources.size() == 2);
  CHECK(back->sources[0].video->fps == 60.0);
  CHECK(back->sources[0].start_offset_ns == 1'234);
  CHECK(back->sources[1].audio->channels == 2);
  CHECK(!back->sources[1].video);
  // Serializing again is byte-identical.
  CHECK(json::serialize(to_json(*back)) == text);
}

TEST(project_save_and_load) {
  std::string dir = drtest::temp_dir("project");
  Project p = sample_project();
  p.state = ProjectState::Complete;
  std::string err;
  REQUIRE(save_project(p, dir, &err));
  CHECK(!std::filesystem::exists(join_path(dir, "project.json.tmp")));
  LoadError lerr;
  auto back = load_project(dir, &lerr);
  REQUIRE(back);
  CHECK(back->state == ProjectState::Complete);
  CHECK(back->find_source("mic") != nullptr);
  std::filesystem::remove_all(dir);
}

TEST(project_loads_v0_fixture) {
  LoadError err;
  auto p = load_project(std::string(DR_TESTDATA_DIR) + "/projects/v0-minimal", &err);
  if (!p) std::fprintf(stderr, "    %s\n", err.message.c_str());
  REQUIRE(p);
  CHECK(p->clock.epoch_host_ns == 17280000000000123LL);
  CHECK(p->sources.size() == 4);
  CHECK(p->find_source("mic")->start_offset_ns == -2'000'000);
  CHECK(p->duration_ns == 600'000'000'000LL);
}

TEST(project_rejects_newer_version) {
  auto doc = to_json(sample_project());
  doc.set("version", 1);
  std::string msg;
  CHECK(!reparse(doc, &msg));
  CHECK(msg.find("$.version") == 0);
}

TEST(project_rejects_bad_fields_with_paths) {
  struct Case {
    const char* source_key;
    json::Value value;
    const char* expected_path;
  };
  Case cases[] = {
      {"file", json::Value("../../etc/passwd"), "$.sources[0].file"},
      {"file", json::Value("/abs/screen.mp4"), "$.sources[0].file"},
      {"file", json::Value("C:\\x.mp4"), "$.sources[0].file"},
      {"kind", json::Value("hologram"), "$.sources[0].kind"},
      {"start_offset_ns", json::Value("soon"), "$.sources[0].start_offset_ns"},
      {"display_id", json::Value("nope"), "$.sources[0].display_id"},
  };
  for (auto& c : cases) {
    auto doc = to_json(sample_project());
    auto* sources = doc.as_object();
    REQUIRE(sources);
    json::Value* src0 = nullptr;
    for (auto& [k, v] : *sources) {
      if (k == "sources") src0 = &(*v.as_array())[0];
    }
    REQUIRE(src0);
    src0->set(c.source_key, c.value);
    std::string msg;
    bool rejected = !reparse(doc, &msg);
    CHECK(rejected);
    if (rejected && msg.find(c.expected_path) != 0) {
      std::fprintf(stderr, "    got: %s\n", msg.c_str());
      CHECK(false);
    }
  }
}

TEST(project_rejects_duplicate_source_ids) {
  auto p = sample_project();
  p.sources[1].id = "screen";
  std::string msg;
  CHECK(!reparse(to_json(p), &msg));
  CHECK(msg.find("duplicate") != std::string::npos);
}

TEST(project_safe_paths) {
  CHECK(is_safe_relative_path("screen.mp4"));
  CHECK(is_safe_relative_path("cursors/0.png"));
  CHECK(!is_safe_relative_path(""));
  CHECK(!is_safe_relative_path("a/../b"));
  CHECK(!is_safe_relative_path("./a"));
  CHECK(!is_safe_relative_path("a//b"));
  CHECK(!is_safe_relative_path("a/"));
  CHECK(!is_safe_relative_path("a\\b"));
  CHECK(!is_safe_relative_path("D:x"));
  // NTFS alternate data streams and Windows device names.
  CHECK(!is_safe_relative_path("screen.mp4:hidden"));
  CHECK(!is_safe_relative_path("cursors/CON"));
  CHECK(!is_safe_relative_path("nul.txt"));
  CHECK(!is_safe_relative_path("Com1.png"));
  CHECK(!is_safe_relative_path("lpt9"));
  CHECK(!is_safe_relative_path("screen.mp4."));
  CHECK(!is_safe_relative_path("cursors /a"));
  CHECK(is_safe_relative_path("console.json"));
  CHECK(is_safe_relative_path("com10.png"));
  CHECK(is_safe_relative_path("cursors/12.png"));
}
