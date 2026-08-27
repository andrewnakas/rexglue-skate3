/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <thread>

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/platform/fpscr.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(
    audio_maxqframes, 16, "Audio",
    "Max buffered audio frames (range 2-64). Lower reduces latency and keeps the guest "
    "audio engine closer to hardware-like lockstep with playback (real hardware double "
    "buffers, ~2); may cause stuttering if too low for the machine.");

// Defined in the SDL driver; gates the same 5-second reporting cadence.
REXCVAR_DECLARE(bool, audio_stats);

REXCVAR_DEFINE_BOOL(
    audio_even_dispatch, true, "Audio",
    "Hold each dispatch of the guest's mixing callback to the 5.333ms guest "
    "frame period instead of running them as fast as credits arrive. Credits "
    "arrive in BURSTS: the device callback consumes a whole device buffer at "
    "once - two guest frames at sample_frames=512, four when a callback runs "
    "late, which on iOS it regularly does (max_callback_gap reads 21ms there "
    "against 11ms on macOS) - so the game's mixer is called several times "
    "within a few hundred microseconds and then not at all for 10-20ms. Its "
    "sources are filled by other guest code on a real-time cadence, so a burst "
    "outruns them and the extra calls mix nothing: an all-zero frame, heard as "
    "a 5.3ms hole. Measured before this: the mixer runs for 50-220us of its "
    "5333us budget and STILL comes up empty for 7% of dispatches on macOS and "
    "32% on iOS, so it is not short of CPU - it is called too early. The "
    "long-run rate is still set by credits, so holding cannot fall behind; it "
    "spends the frame queue's 75-85ms of slack to arrive on time rather than "
    "early. false restores the burst behaviour.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    audio_even_dispatch_floor, 3, "Audio",
    "Frames the driver must still have queued for audio_even_dispatch to hold "
    "a dispatch back. Pacing deliberately runs the guest close to lockstep - "
    "the queue settles at 2-4 frames instead of 14-16 - so a device callback "
    "that arrives late could drain it before the next scheduled dispatch. iOS "
    "does exactly that: its max_callback_gap reads 21ms, two whole periods. "
    "Below this many frames the hold is skipped and the guest is dispatched "
    "immediately, which trades a little of the evenness back for never "
    "underrunning. 0 disables the floor.")
    .range(0, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(audio_worker_thread_priority, 1, "Audio",
                     "Native audio worker thread priority (-2 lowest, -1 below normal, 0 normal, "
                     "1 above normal, 2 highest). The worker runs the guest's audio mixing on a "
                     "~5.3 ms deadline; above-normal keeps it scheduled ahead of the busy "
                     "emulation threads to avoid underrun crackle.")
    .range(-2, 2);

namespace {

uint64_t WorkerNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

class SilentAudioDriver final : public rex::audio::AudioDriver {
 public:
  SilentAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}

  void SubmitFrame(uint32_t /*samples_ptr*/) override {
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }

 private:
  rex::thread::Semaphore* semaphore_;
};

int32_t AudioWorkerNativePriority() {
  switch (std::clamp(REXCVAR_GET(audio_worker_thread_priority), -2, 2)) {
    case -2:
      return rex::thread::ThreadPriority::kLowest;
    case -1:
      return rex::thread::ThreadPriority::kBelowNormal;
    case 1:
      return rex::thread::ThreadPriority::kAboveNormal;
    case 2:
      return rex::thread::ThreadPriority::kHighest;
    case 0:
    default:
      return rex::thread::ThreadPriority::kNormal;
  }
}

}  // namespace

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

namespace rex::audio {

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)), static_cast<uint32_t>(2)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();
  if (worker_thread_->thread()) {
    worker_thread_->thread()->set_priority(AudioWorkerNativePriority());
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    auto result = rex::thread::WaitAny(wait_handles_, rex::countof(wait_handles_), true,
                                       std::chrono::milliseconds(500));
    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }

    if (result.first == thread::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        thread::Wait(resume_event_.get(), false);
      }

      continue;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == rex::thread::WaitResult::kSuccess) {
      auto index = result.second;

      auto global_lock = global_critical_region_.Acquire();
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      global_lock.unlock();

      if (client_callback) {
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        const int32_t floor_frames = REXCVAR_GET(audio_even_dispatch_floor);
        const bool cushion_ok =
            floor_frames <= 0 ||
            static_cast<int32_t>(clients_[index].driver->QueuedFrameCount()) >= floor_frames;
        if (REXCVAR_GET(audio_even_dispatch) && cushion_ok) {
          // 256 samples per guest frame at 48kHz.
          constexpr uint64_t kGuestFrameNs = 5333333;
          uint64_t now = WorkerNowNs();
          if (next_dispatch_ns_ > now) {
            // Never hold longer than one frame period. This is a smoother; a
            // mistake in it must not be able to stall the pipeline.
            const uint64_t hold_ns = std::min(next_dispatch_ns_ - now, kGuestFrameNs);
            std::this_thread::sleep_for(std::chrono::nanoseconds(hold_ns));
            now = WorkerNowNs();
          }
          // Re-base after startup, a pause, or any stall that left the
          // schedule far behind, so it can never bank a burst of its own.
          if (!next_dispatch_ns_ || next_dispatch_ns_ + 4 * kGuestFrameNs < now) {
            next_dispatch_ns_ = now;
          }
          next_dispatch_ns_ += kGuestFrameNs;
        }

        const bool timing = REXCVAR_GET(audio_stats);
        submits_this_dispatch_ = 0;
        submit_was_silent_ = false;
        const uint64_t cb_t0 = timing ? WorkerNowNs() : 0;
        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
        if (timing) {
          const uint64_t us = (WorkerNowNs() - cb_t0) / 1000;
          cb_dispatches_++;
          cb_max_us_ = std::max(cb_max_us_, us);
          if (!submits_this_dispatch_) {
            cb_no_submit_++;
          } else if (submit_was_silent_) {
            cb_silent_n_++;
            cb_silent_us_ += us;
          } else {
            cb_filled_n_++;
            cb_filled_us_ += us;
          }
        }
        // Guest execution can return with host FP exceptions unmasked (the
        // FPSCR emulation leaks MXCSR state); the first host float op on this
        // thread then traps (seen as STATUS_FLOAT_INEXACT_RESULT in the gap
        // logging below). Re-mask before doing any host-side work.
        {
          const uint32_t csr = rex::platform::FPSCRPlatform::getcsr();
          uint32_t masked = csr;
          rex::platform::FPSCRPlatform::InitHostExceptions(masked);
          if (csr != masked) {
            rex::platform::FPSCRPlatform::setcsr(masked);
          }
        }
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: callback returned for client {}", index);
        }
        diag_pump_count++;
      } else {
        REXAPU_DEBUG("AudioWorker: semaphore signaled for client {} but callback is 0", index);
      }

      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (REXCVAR_GET(audio_stats)) {
      const uint64_t now_ns = WorkerNowNs();
      if (!cb_window_start_ns_) {
        cb_window_start_ns_ = now_ns;
      } else if (now_ns - cb_window_start_ns_ >= 5000000000ull) {
        const double win_s = double(now_ns - cb_window_start_ns_) * 1e-9;
        REXAPU_INFO(
            "Audio callback ({:.1f}s): dispatches={} no-submit={} | filled n={} avg={}us | "
            "all-silent n={} avg={}us | max={}us",
            win_s, cb_dispatches_, cb_no_submit_, cb_filled_n_,
            cb_filled_n_ ? cb_filled_us_ / cb_filled_n_ : 0, cb_silent_n_,
            cb_silent_n_ ? cb_silent_us_ / cb_silent_n_ : 0, cb_max_us_);
        cb_window_start_ns_ = now_ns;
        cb_dispatches_ = cb_no_submit_ = cb_filled_n_ = cb_silent_n_ = 0;
        cb_filled_us_ = cb_silent_us_ = cb_max_us_ = 0;
      }
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      rex::thread::Sleep(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked
    // on guest objects (e.g. KeWaitForMultipleObjects).
    // Terminate the thread to break the deadlock.
    worker_thread_->Terminate(0);
    worker_thread_.reset();
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXAPU_DEBUG("AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXAPU_DEBUG("AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    REXAPU_WARN(
        "AudioSystem::RegisterClient: audio driver creation failed with status {:08X}; "
        "using silent audio fallback",
        result);
    driver = new SilentAudioDriver(memory_, client_semaphore);
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXAPU_DEBUG("AudioSystem::SubmitFrame called: index={} samples_ptr={:08X}", index,
                 samples_ptr);
    submit_count++;
  }

  if (REXCVAR_GET(audio_stats)) {
    // Read the guest's buffer BEFORE the driver copies it, so the verdict is
    // about what the game wrote, not about anything we did to it.
    const auto samples = memory_->TranslateVirtual<const float*>(samples_ptr);
    bool silent = true;
    if (samples) {
      for (uint32_t i = 0; i < 256 * 6; i++) {
        if (samples[i] != 0.0f) {
          silent = false;
          break;
        }
      }
    }
    submit_was_silent_ = silent;
    submits_this_dispatch_++;
  }

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);

  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::thread::WaitResult wait_result;
  do {
    wait_result = rex::thread::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::thread::WaitResult::kSuccess);
  assert_true(wait_result == rex::thread::WaitResult::kTimeout);
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      REXAPU_WARN(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}; using silent audio fallback",
          status);
      driver = new SilentAudioDriver(memory_, client_semaphore);
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio
