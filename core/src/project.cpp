#include "dr/project.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

#include "dr/file_util.h"

namespace dr {

namespace {

constexpr size_t kMaxProjectFileBytes = 16 * 1024 * 1024;

struct KindName {
  SourceKind kind;
  const char* name;
};

constexpr std::array<KindName, 6> kKindNames{{
    {SourceKind::Screen, "screen"},
    {SourceKind::Camera, "camera"},
    {SourceKind::Microphone, "microphone"},
    {SourceKind::SystemAudio, "system_audio"},
    {SourceKind::Cursor, "cursor"},
    {SourceKind::Events, "events"},
}};

// Small helper that records the first validation failure with its path.
class Reader {
 public:
  explicit Reader(LoadError* error) : error_(error) {}

  bool ok() const { return ok_; }

  bool fail(const std::string& path, const char* what) {
    if (ok_ && error_) error_->message = path + ": " + what;
    ok_ = false;
    return false;
  }

  bool str(const json::Value& obj, const char* key, const std::string& path, std::string& out,
           bool required = true) {
    const json::Value* v = obj.find(key);
    if (!v) return required ? fail(path + "." + key, "missing") : true;
    const std::string* s = v->as_string();
    if (!s) return fail(path + "." + key, "not a string");
    out = *s;
    return true;
  }

  bool i64(const json::Value& obj, const char* key, const std::string& path, int64_t& out,
           bool required = true) {
    const json::Value* v = obj.find(key);
    if (!v) return required ? fail(path + "." + key, "missing") : true;
    auto i = v->as_int();
    if (!i) return fail(path + "." + key, "not an integer");
    out = *i;
    return true;
  }

  bool i32(const json::Value& obj, const char* key, const std::string& path, int32_t& out,
           int32_t min, int32_t max, bool required = true) {
    int64_t wide = out;
    if (!i64(obj, key, path, wide, required)) return false;
    if (wide < min || wide > max) return fail(path + "." + key, "out of range");
    out = static_cast<int32_t>(wide);
    return true;
  }

  bool f64(const json::Value& obj, const char* key, const std::string& path, double& out,
           double min, double max, bool required = true) {
    const json::Value* v = obj.find(key);
    if (!v) return required ? fail(path + "." + key, "missing") : true;
    auto d = v->as_double();
    if (!d) return fail(path + "." + key, "not a number");
    if (!(*d >= min && *d <= max)) return fail(path + "." + key, "out of range");
    out = *d;
    return true;
  }

  const json::Value* object(const json::Value& obj, const char* key, const std::string& path,
                            bool required = true) {
    const json::Value* v = obj.find(key);
    if (!v) {
      if (required) fail(path + "." + key, "missing");
      return nullptr;
    }
    if (!v->is_object()) {
      fail(path + "." + key, "not an object");
      return nullptr;
    }
    return v;
  }

  const json::Array* array(const json::Value& obj, const char* key, const std::string& path) {
    const json::Value* v = obj.find(key);
    if (!v) {
      fail(path + "." + key, "missing");
      return nullptr;
    }
    const json::Array* a = v->as_array();
    if (!a) fail(path + "." + key, "not an array");
    return a;
  }

 private:
  LoadError* error_;
  bool ok_ = true;
};

constexpr int32_t kMaxDimension = 16384;

}  // namespace

const char* to_string(SourceKind kind) {
  for (const auto& kn : kKindNames) {
    if (kn.kind == kind) return kn.name;
  }
  return "unknown";
}

std::optional<SourceKind> source_kind_from_string(std::string_view s) {
  for (const auto& kn : kKindNames) {
    if (s == kn.name) return kn.kind;
  }
  return std::nullopt;
}

const Source* Project::find_source(std::string_view source_id) const {
  for (const auto& s : sources) {
    if (s.id == source_id) return &s;
  }
  return nullptr;
}

std::string generate_uuid_v4() {
  static thread_local std::mt19937_64 rng([] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937_64(seq);
  }());
  std::array<uint8_t, 16> b{};
  for (size_t i = 0; i < b.size(); i += 8) {
    uint64_t r = rng();
    for (size_t j = 0; j < 8; ++j) b[i + j] = static_cast<uint8_t>(r >> (j * 8));
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
  char out[37];
  std::snprintf(out, sizeof(out),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                b[15]);
  return out;
}

std::string utc_now_rfc3339() {
  std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

Project make_project(AppInfo app, ClockInfo clock) {
  Project p;
  p.id = generate_uuid_v4();
  p.created_at = utc_now_rfc3339();
  p.app = std::move(app);
  p.clock = std::move(clock);
  return p;
}

namespace {

// Windows opens these as devices in any folder, with or without an extension.
bool is_windows_device_name(std::string_view segment) {
  std::string_view stem = segment.substr(0, segment.find('.'));
  auto ieq = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      char c = a[i];
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      if (c != b[i]) return false;
    }
    return true;
  };
  for (std::string_view name : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}) {
    if (ieq(stem, name)) return true;
  }
  if (stem.size() == 4 && (ieq(stem.substr(0, 3), "COM") || ieq(stem.substr(0, 3), "LPT")) &&
      stem[3] >= '0' && stem[3] <= '9') {
    return true;
  }
  return false;
}

}  // namespace

bool is_safe_relative_path(std::string_view path) {
  if (path.empty() || path.size() > 255) return false;
  for (char c : path) {
    // ':' covers drive letters and NTFS alternate data streams.
    if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
  }
  if (path.front() == '/') return false;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    std::string_view part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    // Windows silently strips trailing dots and spaces, so "a." aliases "a".
    if (part.back() == '.' || part.back() == ' ') return false;
    if (is_windows_device_name(part)) return false;
    start = end + 1;
  }
  return true;
}

json::Value to_json(const Project& p) {
  json::Value doc;
  doc.set("format", kProjectFormatName);
  doc.set("version", p.version);
  doc.set("id", p.id);
  doc.set("created_at", p.created_at);
  doc.set("state", p.state == ProjectState::Complete ? "complete" : "recording");

  json::Value app;
  app.set("name", p.app.name);
  app.set("version", p.app.version);
  app.set("platform", p.app.platform);
  app.set("os_version", p.app.os_version);
  doc.set("app", std::move(app));

  json::Value clock;
  clock.set("domain", p.clock.domain);
  clock.set("epoch_host_ns", p.clock.epoch_host_ns);
  doc.set("clock", std::move(clock));

  doc.set("duration_ns", p.duration_ns);

  json::Array displays;
  for (const auto& d : p.displays) {
    json::Value jd;
    jd.set("id", d.id);
    jd.set("width_px", d.width_px);
    jd.set("height_px", d.height_px);
    jd.set("scale", d.scale);
    jd.set("refresh_hz", d.refresh_hz);
    displays.push_back(std::move(jd));
  }
  doc.set("displays", std::move(displays));

  json::Array sources;
  for (const auto& s : p.sources) {
    json::Value js;
    js.set("id", s.id);
    js.set("kind", to_string(s.kind));
    js.set("file", s.file);
    js.set("start_offset_ns", s.start_offset_ns);
    if (!s.display_id.empty()) js.set("display_id", s.display_id);
    if (s.video) {
      json::Value v;
      v.set("codec", s.video->codec);
      v.set("width", s.video->width);
      v.set("height", s.video->height);
      v.set("fps", s.video->fps);
      js.set("video", std::move(v));
    }
    if (s.audio) {
      json::Value a;
      a.set("sample_rate", s.audio->sample_rate);
      a.set("channels", s.audio->channels);
      js.set("audio", std::move(a));
    }
    sources.push_back(std::move(js));
  }
  doc.set("sources", std::move(sources));
  return doc;
}

std::optional<Project> project_from_json(const json::Value& doc, LoadError* error) {
  Reader r(error);
  Project p;
  if (!doc.is_object()) {
    r.fail("$", "not an object");
    return std::nullopt;
  }
  std::string format;
  if (!r.str(doc, "format", "$", format)) return std::nullopt;
  if (format != kProjectFormatName) {
    r.fail("$.format", "not a Demo Recorder project");
    return std::nullopt;
  }
  int64_t version = 0;
  if (!r.i64(doc, "version", "$", version)) return std::nullopt;
  if (version < 0 || version > kProjectFormatVersion) {
    r.fail("$.version", "unsupported version (project was made by a newer Demo Recorder)");
    return std::nullopt;
  }
  p.version = static_cast<int>(version);

  r.str(doc, "id", "$", p.id);
  r.str(doc, "created_at", "$", p.created_at);
  std::string state = "recording";
  r.str(doc, "state", "$", state);
  if (state == "complete") {
    p.state = ProjectState::Complete;
  } else if (state == "recording") {
    p.state = ProjectState::Recording;
  } else {
    r.fail("$.state", "must be \"recording\" or \"complete\"");
  }

  if (const json::Value* app = r.object(doc, "app", "$")) {
    r.str(*app, "name", "$.app", p.app.name, false);
    r.str(*app, "version", "$.app", p.app.version);
    r.str(*app, "platform", "$.app", p.app.platform);
    r.str(*app, "os_version", "$.app", p.app.os_version, false);
  }
  if (const json::Value* clock = r.object(doc, "clock", "$")) {
    r.str(*clock, "domain", "$.clock", p.clock.domain);
    r.i64(*clock, "epoch_host_ns", "$.clock", p.clock.epoch_host_ns);
  }
  r.i64(doc, "duration_ns", "$", p.duration_ns);
  if (p.duration_ns < 0) r.fail("$.duration_ns", "negative");
  if (!r.ok()) return std::nullopt;

  if (const json::Array* displays = r.array(doc, "displays", "$")) {
    for (size_t i = 0; i < displays->size() && r.ok(); ++i) {
      const json::Value& jd = (*displays)[i];
      std::string path = "$.displays[" + std::to_string(i) + "]";
      if (!jd.is_object()) {
        r.fail(path, "not an object");
        break;
      }
      Display d;
      r.str(jd, "id", path, d.id);
      r.i32(jd, "width_px", path, d.width_px, 1, kMaxDimension);
      r.i32(jd, "height_px", path, d.height_px, 1, kMaxDimension);
      r.f64(jd, "scale", path, d.scale, 0.25, 8.0);
      r.f64(jd, "refresh_hz", path, d.refresh_hz, 0.0, 1000.0, false);
      p.displays.push_back(std::move(d));
    }
  }
  if (!r.ok()) return std::nullopt;

  if (const json::Array* sources = r.array(doc, "sources", "$")) {
    for (size_t i = 0; i < sources->size() && r.ok(); ++i) {
      const json::Value& js = (*sources)[i];
      std::string path = "$.sources[" + std::to_string(i) + "]";
      if (!js.is_object()) {
        r.fail(path, "not an object");
        break;
      }
      Source s;
      r.str(js, "id", path, s.id);
      std::string kind;
      if (r.str(js, "kind", path, kind)) {
        auto k = source_kind_from_string(kind);
        if (!k) {
          r.fail(path + ".kind", "unknown source kind");
          break;
        }
        s.kind = *k;
      }
      if (r.str(js, "file", path, s.file) && !is_safe_relative_path(s.file)) {
        r.fail(path + ".file", "unsafe path");
      }
      r.i64(js, "start_offset_ns", path, s.start_offset_ns);
      r.str(js, "display_id", path, s.display_id, false);
      if (const json::Value* v = r.object(js, "video", path, false)) {
        VideoInfo vi;
        r.str(*v, "codec", path + ".video", vi.codec);
        r.i32(*v, "width", path + ".video", vi.width, 1, kMaxDimension);
        r.i32(*v, "height", path + ".video", vi.height, 1, kMaxDimension);
        r.f64(*v, "fps", path + ".video", vi.fps, 1.0, 1000.0);
        s.video = vi;
      }
      if (const json::Value* a = r.object(js, "audio", path, false)) {
        AudioInfo ai;
        r.i32(*a, "sample_rate", path + ".audio", ai.sample_rate, 8000, 384000);
        r.i32(*a, "channels", path + ".audio", ai.channels, 1, 32);
        s.audio = ai;
      }
      if (r.ok() && p.find_source(s.id)) {
        r.fail(path + ".id", "duplicate source id");
      }
      if (r.ok() && s.kind == SourceKind::Screen && !s.display_id.empty()) {
        bool found = false;
        for (const auto& d : p.displays) found = found || d.id == s.display_id;
        if (!found) r.fail(path + ".display_id", "unknown display");
      }
      p.sources.push_back(std::move(s));
    }
  }
  if (!r.ok()) return std::nullopt;
  return p;
}

std::optional<Project> load_project(const std::string& dir, LoadError* error) {
  std::string read_error;
  auto text = read_file(join_path(dir, kProjectFileName), kMaxProjectFileBytes, &read_error);
  if (!text) {
    if (error) error->message = read_error;
    return std::nullopt;
  }
  json::ParseError pe;
  auto doc = json::parse(*text, &pe);
  if (!doc) {
    if (error) error->message = "project.json: " + pe.message + " at byte " + std::to_string(pe.offset);
    return std::nullopt;
  }
  return project_from_json(*doc, error);
}

bool save_project(const Project& project, const std::string& dir, std::string* error) {
  return write_file_atomic(join_path(dir, kProjectFileName), json::serialize(to_json(project)),
                           error);
}

}  // namespace dr
