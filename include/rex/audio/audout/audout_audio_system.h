#pragma once
/**
 * @file        rex/audio/audout/audout_audio_system.h
 * @brief       Audio system backed by libnx audout.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/audio/audio_system.h>

namespace rex::audio::audout {

class AudoutAudioSystem : public AudioSystem {
 public:
  explicit AudoutAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~AudoutAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;
};

}  // namespace rex::audio::audout

#endif  // REX_PLATFORM_SWITCH
