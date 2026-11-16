#include "cursor.h"

#include "dr/clock.h"

namespace drwin {

CursorSampler::CursorSampler(dr::InputRecorder& recorder, RECT monitor, uint32_t output_width,
                             uint32_t output_height)
    : recorder_(recorder),
      monitor_(monitor),
      scale_x_(static_cast<double>(output_width) / (monitor.right - monitor.left)),
      scale_y_(static_cast<double>(output_height) / (monitor.bottom - monitor.top)) {}

CursorSampler::~CursorSampler() { stop(); }

void CursorSampler::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void CursorSampler::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void CursorSampler::run() {
  SetThreadDescription(GetCurrentThread(), L"Demo Recorder cursor");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  // High-resolution waitable timer (Windows 10 1803+); falls back to a
  // regular one, which still ticks at ~1 ms with timeBeginPeriod defaults.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  constexpr LONGLONG kPeriod100ns = 10'000'000 / 120;  // 120 Hz
  uint32_t last_buttons = 0;
  while (running_.load()) {
    const int64_t now = dr::MasterClock::now_ns();
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
      double x = (ci.ptScreenPos.x - monitor_.left) * scale_x_;
      double y = (ci.ptScreenPos.y - monitor_.top) * scale_y_;
      uint32_t buttons = 0;
      // GetAsyncKeyState reports physical buttons; map them to logical
      // (primary/secondary) ones so swapped buttons record correctly.
      const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
      const int primary = swapped ? VK_RBUTTON : VK_LBUTTON;
      const int secondary = swapped ? VK_LBUTTON : VK_RBUTTON;
      if (GetAsyncKeyState(primary) & 0x8000) buttons |= dr::kButtonLeft;
      if (GetAsyncKeyState(secondary) & 0x8000) buttons |= dr::kButtonRight;
      if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) buttons |= dr::kButtonMiddle;
      recorder_.push_cursor(now, x, y, 0, buttons);

      uint32_t changed = buttons ^ last_buttons;
      for (int32_t bit = 0; bit < 3; ++bit) {
        if (changed & (1u << bit)) {
          recorder_.push_event(now, (buttons & (1u << bit)) ? dr::InputEventType::MouseDown : dr::InputEventType::MouseUp, bit);
        }
      }
      last_buttons = buttons;
    }
    if (timer) {
      // Sleep for the rest of the period so the rate stays at 120 Hz.
      const LONGLONG spent = (dr::MasterClock::now_ns() - now) / 100;
      LARGE_INTEGER due;
      due.QuadPart = -(spent < kPeriod100ns ? kPeriod100ns - spent : 1);  // relative
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 100);
    } else {
      Sleep(8);
    }
  }
  if (timer) CloseHandle(timer);
}

}  // namespace drwin
