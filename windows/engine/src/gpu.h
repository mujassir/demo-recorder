// Direct3D 11 device and the BGRA → NV12 converter (spec: Platform matrix →
// "BGRA → NV12/P010 on the GPU (ID3D11VideoProcessor)").
#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace drwin {

struct GpuDevice {
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  std::wstring adapter_name;
};

// Hardware device with video support and multithread protection (Media
// Foundation's encoder uses the same device from its own threads).
GpuDevice create_gpu_device();

// Fixed pool of NV12 textures shared between the converter (writer) and the
// encoder (reader). Slots are claimed and released through an atomic bitmask,
// so neither side ever locks or allocates.
class Nv12Pool {
 public:
  static constexpr uint32_t kMaxSlots = 32;

  Nv12Pool(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t slots);

  // Returns a free slot index or -1 when every texture is in flight.
  int acquire();
  void release(int slot);

  ID3D11Texture2D* texture(int slot) const { return textures_[static_cast<size_t>(slot)].get(); }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t slots() const { return static_cast<uint32_t>(textures_.size()); }

 private:
  std::vector<winrt::com_ptr<ID3D11Texture2D>> textures_;
  std::atomic<uint32_t> free_mask_;
  uint32_t width_;
  uint32_t height_;
};

class Nv12Converter {
 public:
  Nv12Converter(const GpuDevice& gpu, Nv12Pool& pool, uint32_t fps);

  // Queues a GPU blit of `bgra` (any size; scaled to the pool size with the
  // aspect ratio preserved) into pool slot `slot`. Must be called from one
  // thread at a time.
  void convert(ID3D11Texture2D* bgra, int slot);

 private:
  void ensure_processor(uint32_t in_w, uint32_t in_h);
  ID3D11VideoProcessorInputView* input_view(ID3D11Texture2D* tex);
  ID3D11VideoProcessorInputView* copy_input(ID3D11Texture2D* tex);

  GpuDevice gpu_;
  Nv12Pool& pool_;
  uint32_t fps_;
  winrt::com_ptr<ID3D11VideoDevice> video_device_;
  winrt::com_ptr<ID3D11VideoContext> video_context_;
  winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
  winrt::com_ptr<ID3D11VideoProcessor> processor_;
  std::vector<winrt::com_ptr<ID3D11VideoProcessorOutputView>> output_views_;
  // Windows.Graphics.Capture recycles a handful of surfaces; cache their views.
  std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11VideoProcessorInputView>> input_views_;
  winrt::com_ptr<ID3D11Texture2D> copy_tex_;
  winrt::com_ptr<ID3D11VideoProcessorInputView> copy_view_;
  uint32_t copy_w_ = 0;
  uint32_t copy_h_ = 0;
  uint32_t in_w_ = 0;
  uint32_t in_h_ = 0;
};

}  // namespace drwin
