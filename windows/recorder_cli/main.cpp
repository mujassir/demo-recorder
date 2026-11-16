// drrec: P0 recording spike and G0 performance harness for Windows.
//
//   drrec [--seconds N] [--monitor INDEX] [--fps 60] [--max-height 1080]
//         [--output DIR] [--list-monitors]
//
// Without --seconds, records until Enter or Ctrl+C. Prints stats.json (also
// written into the project folder) so the nightly perf job can check budgets.
#include <mfapi.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "drwin/recorder.h"

namespace {

std::atomic<bool> g_stop{false};
HANDLE g_stop_event = nullptr;

BOOL WINAPI on_ctrl(DWORD) {
  g_stop = true;
  if (g_stop_event) SetEvent(g_stop_event);
  return TRUE;
}

std::string narrow(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

int usage() {
  std::fprintf(stderr,
               "usage: drrec [--seconds N] [--monitor INDEX] [--fps N] [--max-height N]\n"
               "             [--output DIR] [--list-monitors]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  drwin::RecordOptions options;
  double seconds = 0;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&]() -> std::wstring {
      if (i + 1 >= argc) {
        std::exit(usage());
      }
      return argv[++i];
    };
    if (a == L"--seconds") seconds = std::stod(next());
    else if (a == L"--monitor") options.monitor_index = std::stoi(next());
    else if (a == L"--fps") options.fps = static_cast<uint32_t>(std::stoul(next()));
    else if (a == L"--max-height") options.max_height = static_cast<uint32_t>(std::stoul(next()));
    else if (a == L"--output") options.library = next();
    else if (a == L"--list-monitors") list = true;
    else return usage();
  }

  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  if (FAILED(MFStartup(MF_VERSION))) {
    std::fprintf(stderr, "Media Foundation is not available (N edition without the Media Feature Pack?)\n");
    return 1;
  }

  int rc = 0;
  try {
    if (list) {
      int i = 0;
      for (const auto& m : drwin::list_monitors()) {
        std::printf("%d: %s %ldx%ld @ %u Hz, scale %.2f%s\n", i++, narrow(m.device).c_str(),
                    m.rect.right - m.rect.left, m.rect.bottom - m.rect.top, m.refresh_hz, m.scale,
                    m.primary ? " (primary)" : "");
      }
    } else {
      g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      SetConsoleCtrlHandler(on_ctrl, TRUE);
      drwin::Recorder recorder(options);
      recorder.start();
      std::fprintf(stderr, "Recording to %s\n", narrow(recorder.folder()).c_str());
      if (seconds > 0) {
        WaitForSingleObject(g_stop_event, static_cast<DWORD>(seconds * 1000));
      } else {
        std::fprintf(stderr, "Press Enter or Ctrl+C to stop.\n");
        HANDLE handles[] = {g_stop_event, GetStdHandle(STD_INPUT_HANDLE)};
        for (;;) {
          DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
          if (r == WAIT_OBJECT_0) break;
          INPUT_RECORD rec;
          DWORD n = 0;
          if (ReadConsoleInputW(handles[1], &rec, 1, &n) && n == 1 && rec.EventType == KEY_EVENT &&
              rec.Event.KeyEvent.bKeyDown && rec.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
            break;
          }
        }
      }
      auto summary = recorder.stop();
      std::fputs(summary.to_json().c_str(), stdout);
      if (summary.frames_dropped_encoder_busy > 0 || summary.input_dropped > 0) rc = 3;
    }
  } catch (const winrt::hresult_error& e) {
    std::fprintf(stderr, "error 0x%08X: %s\n", static_cast<unsigned>(e.code()), winrt::to_string(e.message()).c_str());
    rc = 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    rc = 1;
  }
  MFShutdown();
  return rc;
}
