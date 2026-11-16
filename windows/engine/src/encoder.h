// Hardware H.264 encoder → fragmented MP4 through the Media Foundation sink
// writer (spec: Platform matrix → "Media Foundation hardware MFT as
// fallback"). P1+ adds direct NVENC / AMF / oneVPL paths behind the same
// interface; the MFT path stays as the fallback for every GPU.
#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "dr/spsc_ring.h"
#include "gpu.h"

namespace drwin {

struct EncoderConfig {
  std::wstring path;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 60;
  uint32_t bitrate = 0;
};

struct EncoderStats {
  uint64_t frames_written = 0;
  uint64_t write_failures = 0;
  std::string encoder_name;  // friendly name of the MFT that was picked
  bool hardware = false;
};

class Encoder {
 public:
  // The pool is shared: Media Foundation can release a sample (and so a
  // slot) after the encoder and its owner are gone.
  Encoder(const GpuDevice& gpu, std::shared_ptr<Nv12Pool> pool, const EncoderConfig& config);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  // Called on the capture thread. Hands pool slot `slot` (already converted)
  // to the encoder thread. Returns false if the queue is full; the caller
  // then releases the slot and counts a dropped frame.
  bool submit(int slot, int64_t host_ns);

  // Drains the queue, finalizes the MP4 and joins the encoder thread.
  void finish();

  EncoderStats stats() const;
  int64_t first_frame_host_ns() const { return first_ns_.load(); }
  int64_t last_frame_host_ns() const { return last_ns_.load(); }

 private:
  struct Frame {
    int slot;
    int64_t host_ns;
  };

  void run();
  void write(const Frame& f);
  void describe_encoder();

  GpuDevice gpu_;
  std::shared_ptr<Nv12Pool> pool_;
  EncoderConfig config_;
  UINT reset_token_ = 0;
  winrt::com_ptr<IMFDXGIDeviceManager> device_manager_;
  winrt::com_ptr<IMFSinkWriter> writer_;
  winrt::com_ptr<IMFAsyncCallback> release_callback_;
  DWORD stream_ = 0;

  dr::SpscRing<Frame> queue_;
  std::thread thread_;
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  std::atomic<bool> running_{true};
  std::atomic<int64_t> first_ns_{-1};
  std::atomic<int64_t> last_ns_{-1};
  std::atomic<uint64_t> written_{0};
  std::atomic<uint64_t> failures_{0};
  std::string encoder_name_;
  bool hardware_ = false;
  bool finished_ = false;
};

}  // namespace drwin
