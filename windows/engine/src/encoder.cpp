#include "encoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>

#include <stdexcept>

#include "util.h"

namespace drwin {

namespace {

// Returns a pool slot once Media Foundation drops its last reference to the
// sample that wraps it (IMFTrackedSample). Only then may the converter
// overwrite the texture.
struct SlotReleaser : winrt::implements<SlotReleaser, IMFAsyncCallback> {
  explicit SlotReleaser(Nv12Pool& pool) : pool(pool) {}

  HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
    winrt::com_ptr<IUnknown> state;
    if (SUCCEEDED(result->GetState(state.put()))) {
      // State is an IMFAttributes object carrying the slot index.
      if (auto attrs = state.try_as<IMFAttributes>()) {
        UINT32 slot = 0;
        if (SUCCEEDED(attrs->GetUINT32(kSlotKey, &slot))) pool.release(static_cast<int>(slot));
      }
    }
    return S_OK;
  }

  // {6B1F0E4C-2B7A-4C0E-9E57-6F3A1D2C9B11}
  static constexpr GUID kSlotKey = {0x6b1f0e4c, 0x2b7a, 0x4c0e, {0x9e, 0x57, 0x6f, 0x3a, 0x1d, 0x2c, 0x9b, 0x11}};
  Nv12Pool& pool;
};

void set_codec_value(ICodecAPI* api, const GUID& key, ULONG value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = value;
  api->SetValue(&key, &v);  // best effort: not every encoder supports every knob
}

void set_codec_bool(ICodecAPI* api, const GUID& key, bool value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_BOOL;
  v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  api->SetValue(&key, &v);
}

}  // namespace

Encoder::Encoder(const GpuDevice& gpu, Nv12Pool& pool, const EncoderConfig& config)
    : gpu_(gpu), pool_(pool), config_(config), queue_(64) {
  winrt::check_hresult(MFCreateDXGIDeviceManager(&reset_token_, device_manager_.put()));
  winrt::check_hresult(device_manager_->ResetDevice(gpu_.device.get(), reset_token_));

  winrt::com_ptr<IMFAttributes> attrs;
  winrt::check_hresult(MFCreateAttributes(attrs.put(), 6));
  attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, device_manager_.get());
  attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
  attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
  // Fragmented MP4: a crash loses at most the fragment in progress (spec C-9).
  attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
  // We do our own back-pressure (dropped-frame counting) instead of blocking.
  attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
  winrt::check_hresult(MFCreateSinkWriterFromURL(config_.path.c_str(), nullptr, attrs.get(), writer_.put()));

  winrt::com_ptr<IMFMediaType> out;
  winrt::check_hresult(MFCreateMediaType(out.put()));
  out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  out->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate);
  out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
  MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
  MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, config_.fps, 1);
  MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  winrt::check_hresult(writer_->AddStream(out.get(), &stream_));

  winrt::com_ptr<IMFMediaType> in;
  winrt::check_hresult(MFCreateMediaType(in.put()));
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
  MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, config_.fps, 1);
  MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  winrt::check_hresult(writer_->SetInputMediaType(stream_, in.get(), nullptr));

  // Rate control: VBR around the target, one keyframe per second so the
  // editor can seek quickly and fragments stay independently decodable.
  winrt::com_ptr<ICodecAPI> codec;
  if (SUCCEEDED(writer_->GetServiceForStream(stream_, GUID_NULL, IID_PPV_ARGS(codec.put())))) {
    set_codec_value(codec.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
    set_codec_value(codec.get(), CODECAPI_AVEncCommonMeanBitRate, config_.bitrate);
    set_codec_value(codec.get(), CODECAPI_AVEncCommonMaxBitRate, config_.bitrate * 2);
    set_codec_value(codec.get(), CODECAPI_AVEncMPVGOPSize, config_.fps);
    set_codec_value(codec.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    set_codec_bool(codec.get(), CODECAPI_AVLowLatencyMode, true);
  }
  describe_encoder();

  winrt::check_hresult(writer_->BeginWriting());
  release_callback_ = winrt::make_self<SlotReleaser>(pool_).as<IMFAsyncCallback>();
  thread_ = std::thread([this] { run(); });
}

Encoder::~Encoder() { finish(); }

void Encoder::describe_encoder() {
  // IMFSinkWriterEx exposes the transforms the writer inserted.
  auto ex = writer_.try_as<IMFSinkWriterEx>();
  if (!ex) return;
  for (DWORD i = 0;; ++i) {
    GUID category{};
    winrt::com_ptr<IMFTransform> mft;
    if (FAILED(ex->GetTransformForStream(stream_, i, &category, mft.put()))) break;
    if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
    winrt::com_ptr<IMFAttributes> attrs;
    if (SUCCEEDED(mft->GetAttributes(attrs.put()))) {
      // Only hardware MFTs carry a hardware URL.
      UINT32 url_len = 0;
      hardware_ = SUCCEEDED(attrs->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &url_len));
      WCHAR name[256] = {};
      UINT32 len = 0;
      if (SUCCEEDED(attrs->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), &len))) {
        encoder_name_ = to_utf8(name);
      }
    }
  }
  if (encoder_name_.empty()) encoder_name_ = "unknown";
}

bool Encoder::submit(int slot, int64_t host_ns) {
  if (!queue_.try_push(Frame{slot, host_ns})) return false;
  wake_.notify_one();
  return true;
}

void Encoder::write(const Frame& f) {
  int64_t first = first_ns_.load();
  if (first < 0) {
    first_ns_.store(f.host_ns);
    first = f.host_ns;
  }
  // Media Foundation time is in 100 ns units, relative to the first frame.
  const LONGLONG time = (f.host_ns - first) / 100;
  const LONGLONG duration = 10'000'000LL / config_.fps;

  winrt::com_ptr<IMFMediaBuffer> buffer;
  winrt::com_ptr<IMFTrackedSample> tracked;
  winrt::com_ptr<IMFSample> sample;
  HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pool_.texture(f.slot), 0, FALSE, buffer.put());
  if (SUCCEEDED(hr)) {
    DWORD length = 0;
    if (auto b2 = buffer.try_as<IMF2DBuffer>(); b2 && SUCCEEDED(b2->GetContiguousLength(&length))) {
      buffer->SetCurrentLength(length);
    }
    hr = MFCreateTrackedSample(tracked.put());
  }
  if (SUCCEEDED(hr)) {
    sample = tracked.as<IMFSample>();
    hr = sample->AddBuffer(buffer.get());
  }
  if (SUCCEEDED(hr)) {
    winrt::com_ptr<IMFAttributes> state;
    MFCreateAttributes(state.put(), 1);
    state->SetUINT32(SlotReleaser::kSlotKey, static_cast<UINT32>(f.slot));
    // From here the slot is released when the last sample reference goes.
    hr = tracked->SetAllocator(release_callback_.get(), state.get());
  }
  if (FAILED(hr)) {
    pool_.release(f.slot);
    failures_.fetch_add(1);
    return;
  }
  sample->SetSampleTime(time);
  sample->SetSampleDuration(duration);
  if (FAILED(writer_->WriteSample(stream_, sample.get()))) {
    failures_.fetch_add(1);
  } else {
    written_.fetch_add(1);
    last_ns_.store(f.host_ns);
  }
}

void Encoder::run() {
  SetThreadDescription(GetCurrentThread(), L"Demo Recorder encoder");
  Frame f{};
  for (;;) {
    while (queue_.try_pop(f)) write(f);
    if (!running_.load()) {
      while (queue_.try_pop(f)) write(f);
      return;
    }
    std::unique_lock lock(wake_mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(5));
  }
}

void Encoder::finish() {
  if (finished_) return;
  finished_ = true;
  running_.store(false);
  wake_.notify_one();
  if (thread_.joinable()) thread_.join();
  if (writer_) {
    if (written_.load() > 0) writer_->Finalize();
    writer_ = nullptr;
  }
}

EncoderStats Encoder::stats() const {
  EncoderStats s;
  s.frames_written = written_.load();
  s.write_failures = failures_.load();
  s.encoder_name = encoder_name_;
  s.hardware = hardware_;
  return s;
}

}  // namespace drwin
