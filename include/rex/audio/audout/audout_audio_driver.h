#pragma once
/**
 * @file        rex/audio/audout/audout_audio_driver.h
 * @brief       Guest audio out through libnx audout.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <memory>
#include <mutex>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::audout {

class AudoutAudioDriver : public AudioDriver {
 public:
  AudoutAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~AudoutAudioDriver() override;

  bool Initialize();
  void Shutdown();

  void SubmitFrame(uint32_t frame_ptr) override;
  size_t QueuedFrameCount() const override;

 private:
  // The guest mixes at 48 kHz in six sequential big-endian float channels,
  // 256 samples each. audout takes 48 kHz stereo PCM16, so a frame is
  // downmixed and quantised on its way through.
  static constexpr uint32_t kFrameFrequency = 48000;
  static constexpr uint32_t kFrameChannels = 6;
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameSamples = kFrameChannels * kChannelSamples;
  static constexpr uint32_t kOutputChannels = 2;

  // audout wants each buffer and its size aligned to 0x1000. One frame of
  // stereo PCM16 is 1 KB, so a buffer is mostly padding - which costs 16 KB in
  // total and is not worth packing frames to avoid.
  static constexpr size_t kBufferBytes = 0x1000;
  static constexpr size_t kBufferCount = 4;

  void PumpThread();

  rex::thread::Semaphore* semaphore_ = nullptr;

  AudioOutBuffer buffers_[kBufferCount] = {};
  void* buffer_memory_ = nullptr;

  mutable std::mutex mutex_;
  // Indices of buffers audout is not currently holding.
  uint32_t free_mask_ = 0;

  std::atomic<size_t> queued_frames_{0};
  std::atomic<bool> running_{false};
  std::unique_ptr<rex::thread::Thread> pump_thread_;
  bool initialized_ = false;
};

}  // namespace rex::audio::audout

#endif  // REX_PLATFORM_SWITCH
