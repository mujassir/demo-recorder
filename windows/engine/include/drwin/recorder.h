// Windows recording engine: one recording session.
//
// P0 records the primary (or chosen) monitor plus cursor/click data into a
// project folder that matches docs/project-format.md. Microphone (WASAPI),
// system audio (loopback / process loopback) and camera arrive in P1.
//
// The C# WinUI 3 shell (decision D3) will drive this through a C ABI in
// dr_win_engine.dll; the spike drives it from drrec.exe.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace drwin {

struct MonitorInfo {
  HMONITOR handle = nullptr;
  std::wstring device;  // \\.\DISPLAY1
  RECT rect{};          // physical pixels
  bool primary = false;
  uint32_t refresh_hz = 0;
  double scale = 1.0;
};

std::vector<MonitorInfo> list_monitors();

struct RecordOptions {
  int monitor_index = -1;  // -1 = primary
  uint32_t fps = 60;
  uint32_t max_height = 0;  // 0 = native
  std::wstring library;     // empty = %USERPROFILE%\Videos\Demo Recorder
};

struct RecordSummary {
  std::wstring folder;
  int64_t duration_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t frames_arrived = 0;
  uint64_t frames_written = 0;
  uint64_t frames_skipped_rate_cap = 0;  // faster than the target fps; not a drop
  uint64_t frames_dropped_encoder_busy = 0;
  uint64_t frames_failed = 0;
  uint64_t cursor_rows = 0;
  uint64_t event_rows = 0;
  uint64_t input_dropped = 0;
  std::string encoder;
  bool hardware_encoder = false;
  std::string adapter;
  bool cursor_excluded = false;

  std::string to_json() const;
};

class Recorder {
 public:
  explicit Recorder(RecordOptions options);
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // Throws winrt::hresult_error / std::runtime_error on failure.
  void start();
  RecordSummary stop();

  const std::wstring& folder() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drwin
