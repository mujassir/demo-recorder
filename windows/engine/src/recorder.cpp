#include "drwin/recorder.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <stdexcept>

#include "capture.h"
#include "cursor.h"
#include "dr/clock.h"
#include "dr/file_util.h"
#include "dr/frame_pacer.h"
#include "dr/input_log.h"
#include "dr/json.h"
#include "dr/project.h"
#include "dr/version.h"
#include "encoder.h"
#include "gpu.h"
#include "util.h"

namespace drwin {

// ---------------------------------------------------------------------------
// Monitors

namespace {

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(param);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) return TRUE;
  MonitorInfo info;
  info.handle = monitor;
  info.device = mi.szDevice;
  info.rect = mi.rcMonitor;
  info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    info.refresh_hz = mode.dmDisplayFrequency;
  }
  UINT dpi_x = 96, dpi_y = 96;
  if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
    info.scale = dpi_x / 96.0;
  }
  out->push_back(std::move(info));
  return TRUE;
}

// Even dimensions, optional downscale, and inside H.264 hardware limits.
void output_size(uint32_t in_w, uint32_t in_h, uint32_t max_height, uint32_t& w, uint32_t& h) {
  double dw = in_w, dh = in_h;
  if (max_height && dh > max_height) {
    dw = dw * max_height / dh;
    dh = max_height;
  }
  const double fit = std::min({1.0, 4096.0 / dw, 2304.0 / dh});
  w = static_cast<uint32_t>(dw * fit) & ~1u;
  h = static_cast<uint32_t>(dh * fit) & ~1u;
}

}  // namespace

std::vector<MonitorInfo> list_monitors() {
  std::vector<MonitorInfo> out;
  EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&out));
  std::stable_partition(out.begin(), out.end(), [](const MonitorInfo& m) { return m.primary; });
  return out;
}

// ---------------------------------------------------------------------------
// Summary

std::string RecordSummary::to_json() const {
  dr::json::Value v;
  v.set("duration_ns", duration_ns);
  v.set("width", static_cast<int64_t>(width));
  v.set("height", static_cast<int64_t>(height));
  v.set("frames_arrived", static_cast<int64_t>(frames_arrived));
  v.set("frames_written", static_cast<int64_t>(frames_written));
  v.set("frames_skipped_rate_cap", static_cast<int64_t>(frames_skipped_rate_cap));
  v.set("frames_dropped_encoder_busy", static_cast<int64_t>(frames_dropped_encoder_busy));
  v.set("frames_failed", static_cast<int64_t>(frames_failed));
  v.set("cursor_rows", static_cast<int64_t>(cursor_rows));
  v.set("event_rows", static_cast<int64_t>(event_rows));
  v.set("input_dropped", static_cast<int64_t>(input_dropped));
  v.set("encoder", encoder);
  v.set("hardware_encoder", hardware_encoder);
  v.set("adapter", adapter);
  v.set("cursor_excluded", cursor_excluded);
  return dr::json::serialize(v);
}

// ---------------------------------------------------------------------------
// Recorder

struct Recorder::Impl {
  RecordOptions options;
  MonitorInfo monitor;
  std::wstring folder;
  std::string folder_utf8;
  GpuDevice gpu;
  int64_t epoch_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  dr::FramePacer pacer{60};

  dr::Project project;
  std::unique_ptr<dr::InputRecorder> input;
  std::unique_ptr<CursorSampler> cursor;
  std::shared_ptr<Nv12Pool> pool;
  std::unique_ptr<Nv12Converter> converter;
  std::unique_ptr<Encoder> encoder;
  std::unique_ptr<ScreenCapture> capture;

  // Touched only on the capture callback thread.
  std::atomic<uint64_t> skipped_rate{0};
  std::atomic<uint64_t> dropped_busy{0};
  std::atomic<uint64_t> failed{0};
  bool started = false;

  void on_frame(ID3D11Texture2D* bgra, int64_t host_ns) {
    if (host_ns < epoch_ns) return;  // in flight before recording time 0
    if (!pacer.accept(host_ns)) {
      skipped_rate.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const int slot = pool->acquire();
    if (slot < 0) {
      dropped_busy.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    try {
      converter->convert(bgra, slot);
    } catch (const winrt::hresult_error&) {
      pool->release(slot);
      failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!encoder->submit(slot, host_ns)) {
      pool->release(slot);
      dropped_busy.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
};

Recorder::Recorder(RecordOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
}

Recorder::~Recorder() {
  if (impl_ && impl_->started) {
    try {
      stop();
    } catch (...) {
    }
  }
}

const std::wstring& Recorder::folder() const { return impl_->folder; }

void Recorder::start() {
  Impl& d = *impl_;
  if (!ScreenCapture::is_supported()) {
    throw std::runtime_error("Windows.Graphics.Capture is not supported on this system");
  }
  auto monitors = list_monitors();
  if (monitors.empty()) throw std::runtime_error("no monitors found");
  int index = d.options.monitor_index < 0 ? 0 : d.options.monitor_index;
  if (index >= static_cast<int>(monitors.size())) throw std::runtime_error("monitor index out of range");
  d.monitor = monitors[static_cast<size_t>(index)];

  d.gpu = create_gpu_device();
  const uint32_t fps = std::clamp<uint32_t>(d.options.fps, 1, 240);
  d.pacer = dr::FramePacer(fps);

  d.capture = std::make_unique<ScreenCapture>(d.gpu, d.monitor.handle,
                                              [&d](ID3D11Texture2D* tex, int64_t ns) { d.on_frame(tex, ns); });
  output_size(d.capture->width(), d.capture->height(), d.options.max_height, d.width, d.height);
  const uint32_t bitrate = static_cast<uint32_t>(static_cast<double>(d.width) * d.height * fps * 0.12);

  std::wstring library = d.options.library.empty() ? default_library_folder() : d.options.library;
  d.folder = make_project_folder(library);
  d.folder_utf8 = to_utf8(d.folder);

  d.pool = std::make_shared<Nv12Pool>(d.gpu.device.get(), d.width, d.height, 8);
  d.converter = std::make_unique<Nv12Converter>(d.gpu, *d.pool, fps);
  EncoderConfig ec;
  ec.path = (std::filesystem::path(d.folder) / L"screen.mp4").wstring();
  ec.width = d.width;
  ec.height = d.height;
  ec.fps = fps;
  ec.bitrate = bitrate;
  d.encoder = std::make_unique<Encoder>(d.gpu, d.pool, ec);

  // Recording time 0. P1 moves this to the end of the 3-second countdown.
  d.epoch_ns = dr::MasterClock::now_ns();

  const std::string display_id = to_utf8(d.monitor.device);
  d.project = dr::make_project(dr::AppInfo{"Demo Recorder", DR_CORE_VERSION, "windows", windows_version_string()},
                               dr::ClockInfo{std::string(dr::MasterClock::domain()), d.epoch_ns});
  d.project.displays.push_back(dr::Display{display_id, d.monitor.rect.right - d.monitor.rect.left,
                                           d.monitor.rect.bottom - d.monitor.rect.top, d.monitor.scale,
                                           static_cast<double>(d.monitor.refresh_hz)});
  dr::Source screen;
  screen.id = "screen";
  screen.kind = dr::SourceKind::Screen;
  screen.file = "screen.mp4";
  screen.display_id = display_id;
  screen.video = dr::VideoInfo{"h264", static_cast<int32_t>(d.width), static_cast<int32_t>(d.height),
                               static_cast<double>(fps)};
  d.project.sources.push_back(screen);
  dr::Source cursor_src;
  cursor_src.id = "cursor";
  cursor_src.kind = dr::SourceKind::Cursor;
  cursor_src.file = dr::kCursorFileName;
  d.project.sources.push_back(cursor_src);
  dr::Source events_src;
  events_src.id = "events";
  events_src.kind = dr::SourceKind::Events;
  events_src.file = dr::kEventsFileName;
  d.project.sources.push_back(events_src);
  std::string error;
  // Saved now so a crash still leaves a readable manifest.
  if (!dr::save_project(d.project, d.folder_utf8, &error)) throw std::runtime_error(error);

  d.input = std::make_unique<dr::InputRecorder>(dr::RecordingClock(d.epoch_ns));
  if (!d.input->start(d.folder_utf8, &error)) throw std::runtime_error(error);
  d.cursor = std::make_unique<CursorSampler>(*d.input, d.monitor.rect, d.width, d.height);
  d.cursor->start();
  d.capture->start();
  d.started = true;
}

RecordSummary Recorder::stop() {
  Impl& d = *impl_;
  if (!d.started) throw std::runtime_error("not recording");
  d.started = false;

  d.capture->stop();  // no more frame callbacks after this returns
  d.encoder->finish();
  d.cursor->stop();
  auto input_stats = d.input->stats();
  bool input_ok = d.input->stop();

  // The recording lasts until the user pressed stop, even if the screen was
  // static at the end and no frame arrived (WGC only delivers changes).
  const int64_t stop_ns = dr::MasterClock::now_ns();

  RecordSummary s;
  s.folder = d.folder;
  s.width = d.width;
  s.height = d.height;
  auto es = d.encoder->stats();
  s.frames_arrived = d.capture->frames_arrived();
  s.frames_written = es.frames_written;
  s.frames_skipped_rate_cap = d.skipped_rate.load();
  s.frames_dropped_encoder_busy = d.dropped_busy.load();
  s.frames_failed = d.failed.load() + es.write_failures;
  s.encoder = es.encoder_name;
  s.hardware_encoder = es.hardware;
  s.adapter = to_utf8(d.gpu.adapter_name);
  s.cursor_excluded = d.capture->cursor_excluded();
  s.cursor_rows = input_stats.cursor_rows;
  s.event_rows = input_stats.event_rows;
  s.input_dropped = input_stats.cursor_dropped + input_stats.event_dropped;

  const int64_t first = d.encoder->first_frame_host_ns();
  const int64_t last = d.encoder->last_frame_host_ns();
  s.duration_ns = std::max(stop_ns, last) - d.epoch_ns;
  for (auto& src : d.project.sources) {
    if (src.id == "screen" && first >= 0) src.start_offset_ns = first - d.epoch_ns;
  }
  d.project.duration_ns = std::max<int64_t>(0, s.duration_ns);
  d.project.state = dr::ProjectState::Complete;

  // Release GPU objects. Samples still held by Media Foundation keep the
  // pool alive through their release callback, so the order is not critical.
  d.capture.reset();
  d.encoder.reset();
  d.converter.reset();
  d.pool.reset();

  std::string error;
  if (!dr::save_project(d.project, d.folder_utf8, &error)) throw std::runtime_error(error);
  dr::write_file_atomic(dr::join_path(d.folder_utf8, "stats.json"), s.to_json());
  if (!input_ok) throw std::runtime_error("failed to write cursor/event logs");
  if (es.frames_written == 0) throw std::runtime_error("no frames were captured");
  return s;
}

}  // namespace drwin
