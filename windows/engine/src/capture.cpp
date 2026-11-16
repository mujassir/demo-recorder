#include "capture.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <thread>

namespace drwin {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace {
constexpr auto kPixelFormat = wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int32_t kBuffers = 3;
}  // namespace

bool ScreenCapture::is_supported() { return wgc::GraphicsCaptureSession::IsSupported(); }

ScreenCapture::ScreenCapture(const GpuDevice& gpu, HMONITOR monitor, FrameHandler handler)
    : handler_(std::move(handler)) {
  auto dxgi = gpu.device.as<IDXGIDevice>();
  winrt::com_ptr<::IInspectable> inspectable;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
  device_ = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

  auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::IGraphicsCaptureItem>(),
                                                 winrt::put_abi(item_)));
  size_ = item_.Size();
}

ScreenCapture::~ScreenCapture() { stop(); }

void ScreenCapture::start() {
  pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, kPixelFormat, kBuffers, size_);
  revoker_ = pool_.FrameArrived(winrt::auto_revoke, {this, &ScreenCapture::on_frame});
  session_ = pool_.CreateCaptureSession(item_);
  // The cursor is recorded as data and redrawn by the editor (spec I-1).
  try {
    session_.IsCursorCaptureEnabled(false);  // Windows 10 2004+
    cursor_excluded_ = true;
  } catch (const winrt::hresult_error&) {
  }
  // Windows 11 can drop the yellow capture border.
  try {
    session_.IsBorderRequired(false);
    border_disabled_ = true;
  } catch (const winrt::hresult_error&) {
  }
  session_.StartCapture();
}

void ScreenCapture::stop() {
  if (stopping_.exchange(true)) return;
  revoker_.revoke();
  if (session_) session_.Close();
  if (pool_) pool_.Close();
  // A callback that started before the revoke may still be running.
  while (in_callback_.load() > 0) std::this_thread::yield();
  session_ = nullptr;
  pool_ = nullptr;
}

void ScreenCapture::on_frame(wgc::Direct3D11CaptureFramePool const& sender,
                             winrt::Windows::Foundation::IInspectable const&) {
  in_callback_.fetch_add(1);
  struct Leave {
    std::atomic<int>& n;
    ~Leave() { n.fetch_sub(1); }
  } leave{in_callback_};
  if (stopping_.load()) return;

  wgc::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
  if (!frame) return;
  arrived_.fetch_add(1);

  // SystemRelativeTime is QPC in 100 ns units: the master clock.
  const int64_t host_ns = frame.SystemRelativeTime().count() * 100;

  auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;
  winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
  handler_(texture.get(), host_ns);

  // Resolution changed mid-recording: recreate the pool at the new size. The
  // converter rescales into the fixed output size.
  auto content = frame.ContentSize();
  frame.Close();
  if (content.Width != size_.Width || content.Height != size_.Height) {
    size_ = content;
    sender.Recreate(device_, kPixelFormat, kBuffers, size_);
  }
}

}  // namespace drwin
