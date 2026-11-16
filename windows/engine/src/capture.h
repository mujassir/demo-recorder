// Windows.Graphics.Capture source for one monitor (spec C-1, P0: full screen).
#pragma once

#include <d3d11.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstdint>
#include <functional>

#include "gpu.h"

namespace drwin {

class ScreenCapture {
 public:
  // Runs on the frame pool's worker thread, one frame at a time. The texture
  // is only valid during the call (the pool recycles it afterwards).
  using FrameHandler = std::function<void(ID3D11Texture2D* bgra, int64_t host_ns)>;

  ScreenCapture(const GpuDevice& gpu, HMONITOR monitor, FrameHandler handler);
  ~ScreenCapture();
  ScreenCapture(const ScreenCapture&) = delete;
  ScreenCapture& operator=(const ScreenCapture&) = delete;

  static bool is_supported();

  void start();
  // Stops delivery and waits for an in-flight callback to return.
  void stop();

  uint32_t width() const { return static_cast<uint32_t>(size_.Width); }
  uint32_t height() const { return static_cast<uint32_t>(size_.Height); }
  uint64_t frames_arrived() const { return arrived_.load(); }
  bool cursor_excluded() const { return cursor_excluded_; }
  bool border_disabled() const { return border_disabled_; }

 private:
  void on_frame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
                winrt::Windows::Foundation::IInspectable const&);

  FrameHandler handler_;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker revoker_;
  winrt::Windows::Graphics::SizeInt32 size_{};
  std::atomic<bool> stopping_{false};
  std::atomic<int> in_callback_{0};
  std::atomic<uint64_t> arrived_{0};
  bool cursor_excluded_ = false;
  bool border_disabled_ = false;
};

}  // namespace drwin
