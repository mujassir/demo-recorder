#include "gpu.h"

#include <d3d10.h>

#include <algorithm>
#include <stdexcept>

namespace drwin {

GpuDevice create_gpu_device() {
  GpuDevice gpu;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                         ARRAYSIZE(levels), D3D11_SDK_VERSION, gpu.device.put(),
                                         nullptr, gpu.context.put()));
  auto mt = gpu.device.as<ID3D10Multithread>();
  mt->SetMultithreadProtected(TRUE);

  auto dxgi = gpu.device.as<IDXGIDevice>();
  winrt::com_ptr<IDXGIAdapter> adapter;
  if (SUCCEEDED(dxgi->GetAdapter(adapter.put()))) {
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) gpu.adapter_name = desc.Description;
  }
  return gpu;
}

// ---------------------------------------------------------------------------

Nv12Pool::Nv12Pool(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t slots)
    : free_mask_(0), width_(width), height_(height) {
  slots = std::clamp<uint32_t>(slots, 2, kMaxSlots);
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  // Render target for the video processor; shader resource lets the hardware
  // encoder MFT read it without a copy on every vendor driver we target.
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  textures_.resize(slots);
  for (auto& t : textures_) {
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, t.put()));
  }
  free_mask_.store(slots == 32 ? 0xFFFFFFFFu : ((1u << slots) - 1));
}

int Nv12Pool::acquire() {
  uint32_t mask = free_mask_.load(std::memory_order_acquire);
  while (mask != 0) {
    unsigned long bit;
    _BitScanForward(&bit, mask);
    uint32_t next = mask & ~(1u << bit);
    if (free_mask_.compare_exchange_weak(mask, next, std::memory_order_acq_rel)) {
      return static_cast<int>(bit);
    }
  }
  return -1;
}

void Nv12Pool::release(int slot) {
  if (slot < 0) return;
  free_mask_.fetch_or(1u << static_cast<uint32_t>(slot), std::memory_order_release);
}

// ---------------------------------------------------------------------------

Nv12Converter::Nv12Converter(const GpuDevice& gpu, Nv12Pool& pool, uint32_t fps)
    : gpu_(gpu), pool_(pool), fps_(fps) {
  video_device_ = gpu_.device.as<ID3D11VideoDevice>();
  video_context_ = gpu_.context.as<ID3D11VideoContext>();
}

void Nv12Converter::ensure_processor(uint32_t in_w, uint32_t in_h) {
  if (processor_ && in_w == in_w_ && in_h == in_h_) return;
  processor_ = nullptr;
  enumerator_ = nullptr;
  copy_view_ = nullptr;
  copy_tex_ = nullptr;
  output_views_.clear();
  input_views_.clear();

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputFrameRate = {fps_, 1};
  content.InputWidth = in_w;
  content.InputHeight = in_h;
  content.OutputFrameRate = {fps_, 1};
  content.OutputWidth = pool_.width();
  content.OutputHeight = pool_.height();
  content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
  winrt::check_hresult(video_device_->CreateVideoProcessorEnumerator(&content, enumerator_.put()));
  winrt::check_hresult(video_device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()));

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
  ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_views_.resize(pool_.slots());
  for (uint32_t i = 0; i < pool_.slots(); ++i) {
    winrt::check_hresult(video_device_->CreateVideoProcessorOutputView(
        pool_.texture(static_cast<int>(i)), enumerator_.get(), &ov, output_views_[i].put()));
  }

  // Desktop pixels are full-range sRGB; encoders expect BT.709 studio range.
  if (auto vc1 = video_context_.try_as<ID3D11VideoContext1>()) {
    vc1->VideoProcessorSetStreamColorSpace1(processor_.get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vc1->VideoProcessorSetOutputColorSpace1(processor_.get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  } else {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range = 0;  // full
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.YCbCr_Matrix = 1;  // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &in);
    video_context_->VideoProcessorSetOutputColorSpace(processor_.get(), &out);
  }
  video_context_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);

  // Letterbox if the captured surface's aspect ratio differs from the output.
  RECT src{0, 0, static_cast<LONG>(in_w), static_cast<LONG>(in_h)};
  double sx = static_cast<double>(pool_.width()) / in_w;
  double sy = static_cast<double>(pool_.height()) / in_h;
  double s = std::min(sx, sy);
  LONG dw = static_cast<LONG>(in_w * s);
  LONG dh = static_cast<LONG>(in_h * s);
  LONG dx = (static_cast<LONG>(pool_.width()) - dw) / 2;
  LONG dy = (static_cast<LONG>(pool_.height()) - dh) / 2;
  RECT dst{dx, dy, dx + dw, dy + dh};
  RECT full{0, 0, static_cast<LONG>(pool_.width()), static_cast<LONG>(pool_.height())};
  video_context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &src);
  video_context_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &dst);
  video_context_->VideoProcessorSetOutputTargetRect(processor_.get(), TRUE, &full);
  D3D11_VIDEO_COLOR black{};
  black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
  video_context_->VideoProcessorSetOutputBackgroundColor(processor_.get(), TRUE, &black);

  in_w_ = in_w;
  in_h_ = in_h;
}

ID3D11VideoProcessorInputView* Nv12Converter::input_view(ID3D11Texture2D* tex) {
  auto it = input_views_.find(tex);
  if (it != input_views_.end()) return it->second.get();
  // Bound the cache: stale entries appear when the capture pool is recreated.
  if (input_views_.size() > 16) input_views_.clear();
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
  iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  winrt::com_ptr<ID3D11VideoProcessorInputView> view;
  HRESULT hr = video_device_->CreateVideoProcessorInputView(tex, enumerator_.get(), &iv, view.put());
  if (FAILED(hr)) return nullptr;  // caller falls back to a GPU copy
  auto* raw = view.get();
  input_views_.emplace(tex, std::move(view));
  return raw;
}

ID3D11VideoProcessorInputView* Nv12Converter::copy_input(ID3D11Texture2D* tex) {
  // Some drivers hand out capture surfaces without the bind flags the video
  // processor needs. Copy into a texture we own (GPU to GPU, no readback).
  D3D11_TEXTURE2D_DESC desc{};
  tex->GetDesc(&desc);
  if (!copy_tex_ || copy_w_ != desc.Width || copy_h_ != desc.Height) {
    copy_view_ = nullptr;
    copy_tex_ = nullptr;
    D3D11_TEXTURE2D_DESC own = desc;
    own.MipLevels = 1;
    own.ArraySize = 1;
    own.Usage = D3D11_USAGE_DEFAULT;
    own.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    own.CPUAccessFlags = 0;
    own.MiscFlags = 0;
    winrt::check_hresult(gpu_.device->CreateTexture2D(&own, nullptr, copy_tex_.put()));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    winrt::check_hresult(
        video_device_->CreateVideoProcessorInputView(copy_tex_.get(), enumerator_.get(), &iv, copy_view_.put()));
    copy_w_ = desc.Width;
    copy_h_ = desc.Height;
  }
  gpu_.context->CopyResource(copy_tex_.get(), tex);
  return copy_view_.get();
}

void Nv12Converter::convert(ID3D11Texture2D* bgra, int slot) {
  D3D11_TEXTURE2D_DESC desc{};
  bgra->GetDesc(&desc);
  ensure_processor(desc.Width, desc.Height);
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view(bgra);
  if (!stream.pInputSurface) stream.pInputSurface = copy_input(bgra);
  winrt::check_hresult(video_context_->VideoProcessorBlt(
      processor_.get(), output_views_[static_cast<size_t>(slot)].get(), 0, 1, &stream));
}

}  // namespace drwin
