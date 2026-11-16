#include "dr/dr_core.h"

#include <memory>
#include <new>
#include <string>

#include "dr/clock.h"
#include "dr/input_log.h"
#include "dr/project.h"
#include "dr/version.h"

struct dr_project {
  dr::Project project;
};

struct dr_input_recorder {
  explicit dr_input_recorder(int64_t epoch) : recorder(dr::RecordingClock(epoch)) {}
  dr::InputRecorder recorder;
};

namespace {

thread_local std::string g_last_error;

dr_status fail(dr_status status, std::string message) {
  g_last_error = std::move(message);
  return status;
}

dr_status ok() {
  g_last_error.clear();
  return DR_OK;
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

dr::SourceKind to_kind(dr_source_kind k) {
  switch (k) {
    case DR_SOURCE_SCREEN: return dr::SourceKind::Screen;
    case DR_SOURCE_CAMERA: return dr::SourceKind::Camera;
    case DR_SOURCE_MICROPHONE: return dr::SourceKind::Microphone;
    case DR_SOURCE_SYSTEM_AUDIO: return dr::SourceKind::SystemAudio;
    case DR_SOURCE_CURSOR: return dr::SourceKind::Cursor;
    case DR_SOURCE_EVENTS: return dr::SourceKind::Events;
  }
  return dr::SourceKind::Screen;
}

bool valid_kind(int k) { return k >= DR_SOURCE_SCREEN && k <= DR_SOURCE_EVENTS; }

dr_status add_source(dr_project* p, dr::Source s) {
  if (s.id.empty()) return fail(DR_ERR_INVALID_ARGUMENT, "source id is empty");
  if (!dr::is_safe_relative_path(s.file)) {
    return fail(DR_ERR_INVALID_ARGUMENT, "source file must be a relative path inside the project");
  }
  if (p->project.find_source(s.id)) return fail(DR_ERR_INVALID_ARGUMENT, "duplicate source id");
  p->project.sources.push_back(std::move(s));
  return ok();
}

}  // namespace

extern "C" {

const char* dr_core_version(void) { return DR_CORE_VERSION; }
int32_t dr_project_format_version(void) { return dr::kProjectFormatVersion; }
const char* dr_last_error(void) { return g_last_error.c_str(); }

uint64_t dr_clock_now_ticks(void) { return dr::MasterClock::now_ticks(); }
int64_t dr_clock_now_ns(void) { return dr::MasterClock::now_ns(); }
int64_t dr_clock_ticks_to_ns(uint64_t ticks) {
  return dr::ticks_to_ns(ticks, dr::MasterClock::timebase());
}
const char* dr_clock_domain(void) {
  // domain() returns a string literal, so data() is NUL-terminated.
  return dr::MasterClock::domain().data();
}

dr_project* dr_project_create(const char* platform, const char* app_version,
                              const char* os_version, int64_t epoch_host_ns) {
  auto* p = new (std::nothrow) dr_project;
  if (!p) return nullptr;
  dr::AppInfo app;
  app.platform = str(platform);
  app.version = str(app_version);
  app.os_version = str(os_version);
  dr::ClockInfo clock{std::string(dr::MasterClock::domain()), epoch_host_ns};
  p->project = dr::make_project(std::move(app), std::move(clock));
  ok();
  return p;
}

void dr_project_free(dr_project* project) { delete project; }

const char* dr_project_id(const dr_project* project) {
  return project ? project->project.id.c_str() : "";
}

dr_status dr_project_add_display(dr_project* project, const char* display_id, int32_t width_px,
                                 int32_t height_px, double scale, double refresh_hz) {
  if (!project || !display_id || !*display_id) return fail(DR_ERR_INVALID_ARGUMENT, "invalid display");
  if (width_px <= 0 || height_px <= 0 || !(scale > 0)) {
    return fail(DR_ERR_INVALID_ARGUMENT, "invalid display size");
  }
  project->project.displays.push_back(
      dr::Display{display_id, width_px, height_px, scale, refresh_hz > 0 ? refresh_hz : 0.0});
  return ok();
}

dr_status dr_project_add_video_source(dr_project* project, const char* source_id,
                                      dr_source_kind kind, const char* file,
                                      const char* display_id, const char* codec, int32_t width,
                                      int32_t height, double fps, int64_t start_offset_ns) {
  if (!project || !valid_kind(kind)) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  if (width <= 0 || height <= 0 || !(fps > 0)) {
    return fail(DR_ERR_INVALID_ARGUMENT, "invalid video format");
  }
  dr::Source s;
  s.id = str(source_id);
  s.kind = to_kind(kind);
  s.file = str(file);
  s.display_id = str(display_id);
  s.start_offset_ns = start_offset_ns;
  s.video = dr::VideoInfo{str(codec), width, height, fps};
  return add_source(project, std::move(s));
}

dr_status dr_project_add_audio_source(dr_project* project, const char* source_id,
                                      dr_source_kind kind, const char* file, int32_t sample_rate,
                                      int32_t channels, int64_t start_offset_ns) {
  if (!project || !valid_kind(kind)) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  if (sample_rate <= 0 || channels <= 0) return fail(DR_ERR_INVALID_ARGUMENT, "invalid audio format");
  dr::Source s;
  s.id = str(source_id);
  s.kind = to_kind(kind);
  s.file = str(file);
  s.start_offset_ns = start_offset_ns;
  s.audio = dr::AudioInfo{sample_rate, channels};
  return add_source(project, std::move(s));
}

dr_status dr_project_add_data_source(dr_project* project, const char* source_id,
                                     dr_source_kind kind, const char* file) {
  if (!project || !valid_kind(kind)) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  dr::Source s;
  s.id = str(source_id);
  s.kind = to_kind(kind);
  s.file = str(file);
  return add_source(project, std::move(s));
}

dr_status dr_project_set_source_start(dr_project* project, const char* source_id,
                                      int64_t start_offset_ns) {
  if (!project || !source_id) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  for (auto& s : project->project.sources) {
    if (s.id == source_id) {
      s.start_offset_ns = start_offset_ns;
      return ok();
    }
  }
  return fail(DR_ERR_INVALID_ARGUMENT, "unknown source id");
}

void dr_project_set_duration_ns(dr_project* project, int64_t duration_ns) {
  if (project) project->project.duration_ns = duration_ns < 0 ? 0 : duration_ns;
}

void dr_project_mark_complete(dr_project* project) {
  if (project) project->project.state = dr::ProjectState::Complete;
}

dr_status dr_project_save(const dr_project* project, const char* dir) {
  if (!project || !dir) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  std::string error;
  if (!dr::save_project(project->project, dir, &error)) return fail(DR_ERR_IO, error);
  return ok();
}

dr_project* dr_project_load(const char* dir) {
  if (!dir) {
    fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
    return nullptr;
  }
  dr::LoadError error;
  auto loaded = dr::load_project(dir, &error);
  if (!loaded) {
    fail(DR_ERR_FORMAT, error.message);
    return nullptr;
  }
  auto* p = new (std::nothrow) dr_project;
  if (!p) return nullptr;
  p->project = std::move(*loaded);
  ok();
  return p;
}

int64_t dr_project_duration_ns(const dr_project* project) {
  return project ? project->project.duration_ns : 0;
}

size_t dr_project_source_count(const dr_project* project) {
  return project ? project->project.sources.size() : 0;
}

dr_input_recorder* dr_input_recorder_start(const char* dir, int64_t epoch_host_ns) {
  if (!dir) {
    fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
    return nullptr;
  }
  auto r = std::make_unique<dr_input_recorder>(epoch_host_ns);
  std::string error;
  if (!r->recorder.start(dir, &error)) {
    fail(DR_ERR_IO, error);
    return nullptr;
  }
  ok();
  return r.release();
}

void dr_input_recorder_push_cursor(dr_input_recorder* recorder, int64_t host_ns, double x,
                                   double y, int32_t shape_id, uint32_t buttons) {
  if (recorder) recorder->recorder.push_cursor(host_ns, x, y, shape_id, buttons);
}

void dr_input_recorder_push_event(dr_input_recorder* recorder, int64_t host_ns,
                                  dr_input_event_type type, int32_t code) {
  if (!recorder || type < DR_EVENT_MOUSE_DOWN || type > DR_EVENT_KEY_UP) return;
  // Key identity is never stored (privacy rule, spec I-2).
  if (type == DR_EVENT_KEY_DOWN || type == DR_EVENT_KEY_UP) code = 0;
  recorder->recorder.push_event(host_ns, static_cast<dr::InputEventType>(type), code);
}

dr_input_stats dr_input_recorder_stats(const dr_input_recorder* recorder) {
  dr_input_stats out{};
  if (!recorder) return out;
  auto st = recorder->recorder.stats();
  out.cursor_rows = st.cursor_rows;
  out.event_rows = st.event_rows;
  out.cursor_dropped = st.cursor_dropped;
  out.event_dropped = st.event_dropped;
  return out;
}

dr_status dr_input_recorder_stop(dr_input_recorder* recorder) {
  if (!recorder) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  bool stopped = recorder->recorder.stop();
  delete recorder;
  if (!stopped) return fail(DR_ERR_IO, "failed to write input logs");
  return ok();
}

dr_status dr_recover_row_log(const char* path, uint64_t* rows_kept) {
  if (!path) return fail(DR_ERR_INVALID_ARGUMENT, "invalid argument");
  std::string error;
  auto r = dr::recover_row_log(path, &error);
  if (!r) return fail(DR_ERR_FORMAT, error);
  if (rows_kept) *rows_kept = r->rows_kept;
  return ok();
}

}  // extern "C"
