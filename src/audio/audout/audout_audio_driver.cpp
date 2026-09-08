/**
 * @file        audio/audout/audout_audio_driver.cpp
 * @brief       Guest audio out through libnx audout.
 *
 * audren is the richer of Horizon's two audio paths - submixes, effects,
 * surround - and none of that is wanted here: the guest has already mixed its
 * six channels and the only remaining job is to get samples to the speakers.
 * audout is the thinner path and does exactly that.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/audio/audout/audout_audio_driver.h>

#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/audio/conversion.h>
#include <rex/logging.h>

namespace rex::audio::audout {

AudoutAudioDriver::AudoutAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

AudoutAudioDriver::~AudoutAudioDriver() {
  Shutdown();
}

bool AudoutAudioDriver::Initialize() {
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    REXAPU_ERROR("audoutInitialize failed (rc=0x{:x})", rc);
    return false;
  }
  initialized_ = true;

  // Both are fixed by the service, but read them back rather than assume: a
  // mismatch here would be a silent pitch shift rather than an error.
  const u32 rate = audoutGetSampleRate();
  const u32 channels = audoutGetChannelCount();
  if (rate != kFrameFrequency || channels != kOutputChannels) {
    REXAPU_WARN("audout offers {} Hz / {} channels, expected {} / {}", rate, channels,
                kFrameFrequency, kOutputChannels);
  }

  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    REXAPU_ERROR("audoutStartAudioOut failed (rc=0x{:x})", rc);
    Shutdown();
    return false;
  }

  // One allocation for all four buffers: audout requires each to start on a
  // 0x1000 boundary, and a single aligned block gives that for free.
  buffer_memory_ = memalign(kBufferBytes, kBufferBytes * kBufferCount);
  if (!buffer_memory_) {
    REXAPU_ERROR("could not allocate {} KB of audio buffers",
                 (kBufferBytes * kBufferCount) / 1024);
    Shutdown();
    return false;
  }
  std::memset(buffer_memory_, 0, kBufferBytes * kBufferCount);

  for (size_t i = 0; i < kBufferCount; ++i) {
    buffers_[i].next = nullptr;
    buffers_[i].buffer = static_cast<uint8_t*>(buffer_memory_) + i * kBufferBytes;
    buffers_[i].buffer_size = kBufferBytes;
    buffers_[i].data_size = kChannelSamples * kOutputChannels * sizeof(int16_t);
    buffers_[i].data_offset = 0;
    free_mask_ |= (1u << i);
  }

  running_.store(true, std::memory_order_release);
  pump_thread_ = rex::thread::Thread::Create({}, [this]() { PumpThread(); });
  if (!pump_thread_) {
    REXAPU_ERROR("could not start the audio pump thread");
    Shutdown();
    return false;
  }
  pump_thread_->set_name("Audio Pump");

  // The mixer is held to a steady cadence by audio_even_dispatch, which needs a
  // driver that reports a queue. Start it with every buffer free so the guest
  // may run ahead by the whole ring before it is made to wait.
  SetAutoDeviceSampleFrames(int32_t(kChannelSamples));
  // Warn level so it survives the shipped log_level: silence from this driver
  // is indistinguishable from a driver that never started, and "no sound" is
  // the report that arrives when either happens.
  REXAPU_WARN("audout ready: {} Hz, {} channels, {} buffers of {} frames", kFrameFrequency,
              kOutputChannels, kBufferCount, kChannelSamples);
  return true;
}

void AudoutAudioDriver::Shutdown() {
  running_.store(false, std::memory_order_release);
  if (pump_thread_) {
    // The pump wakes at least every 100 ms, so this joins promptly without
    // needing anything to interrupt it.
    rex::thread::Wait(pump_thread_.get(), false, std::chrono::milliseconds(500));
    pump_thread_.reset();
  }
  if (initialized_) {
    audoutStopAudioOut();
    audoutExit();
    initialized_ = false;
  }
  if (buffer_memory_) {
    free(buffer_memory_);
    buffer_memory_ = nullptr;
  }
  free_mask_ = 0;
}

void AudoutAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  if (!initialized_) {
    return;
  }

  // Says whether the guest is feeding this driver at all. A frame count that
  // stays at zero means the mixer never reached us, which is a different
  // problem from one where the frames arrive and do not make a sound.
  {
    static std::atomic<uint64_t> submitted{0};
    const uint64_t n = submitted.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 2000) == 0) {
      REXAPU_WARN("audout: {} guest frame(s) submitted", n);
    }
  }

  int index = -1;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (free_mask_) {
      index = __builtin_ctz(free_mask_);
      free_mask_ &= ~(1u << index);
    }
  }
  if (index < 0) {
    // Every buffer is with audout. Dropping this frame is the right answer:
    // the alternative is blocking the guest's audio worker, which on three
    // cores costs a frame everywhere else too. The mixer will be along with
    // another in five milliseconds.
    REXAPU_DEBUG("audout: no free buffer, dropping a frame");
    return;
  }

  const auto* input = memory_->TranslateVirtual<const float*>(frame_ptr);

  // Six sequential big-endian channels in, interleaved stereo little-endian
  // float out. The conversion is shared with every other backend.
  float stereo[kChannelSamples * kOutputChannels];
  conversion::sequential_6_BE_to_interleaved_2_LE(stereo, input, kChannelSamples);

  auto* output = static_cast<int16_t*>(buffers_[index].buffer);
  for (uint32_t i = 0; i < kChannelSamples * kOutputChannels; ++i) {
    // Clamp before scaling: the guest mixes in float and can legitimately
    // exceed unity, which would wrap rather than clip if it were simply cast.
    const float sample = std::clamp(stereo[i], -1.0f, 1.0f);
    output[i] = int16_t(sample * 32767.0f);
  }

  const Result rc = audoutAppendAudioOutBuffer(&buffers_[index]);
  if (R_FAILED(rc)) {
    REXAPU_WARN("audoutAppendAudioOutBuffer failed (rc=0x{:x})", rc);
    std::lock_guard<std::mutex> guard(mutex_);
    free_mask_ |= (1u << index);
    return;
  }
  queued_frames_.fetch_add(1, std::memory_order_release);
}

size_t AudoutAudioDriver::QueuedFrameCount() const {
  return queued_frames_.load(std::memory_order_acquire);
}

void AudoutAudioDriver::PumpThread() {
  while (running_.load(std::memory_order_acquire)) {
    AudioOutBuffer* released = nullptr;
    u32 released_count = 0;
    // A timeout rather than an indefinite wait, so shutdown does not depend on
    // audio still playing.
    const Result rc = audoutWaitPlayFinish(&released, &released_count, 100'000'000ull);
    if (R_FAILED(rc) || !released_count) {
      continue;
    }

    {
      std::lock_guard<std::mutex> guard(mutex_);
      for (size_t i = 0; i < kBufferCount; ++i) {
        if (&buffers_[i] == released) {
          free_mask_ |= (1u << i);
          break;
        }
      }
    }
    queued_frames_.fetch_sub(1, std::memory_order_release);

    // Tells the guest's audio worker a slot has opened. This is the whole
    // reason the driver has a thread: without it the mixer would have to
    // discover the free buffer by polling.
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
}

}  // namespace rex::audio::audout

#endif  // REX_PLATFORM_SWITCH
