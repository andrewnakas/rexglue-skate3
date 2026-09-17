/**
 * @file        audio/audout/audout_audio_system.cpp
 * @brief       Audio system backed by libnx audout.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/audio/audout/audout_audio_system.h>

#include <rex/assert.h>
#include <rex/audio/audout/audout_audio_driver.h>
#include <rex/audio/flags.h>

namespace rex::audio::audout {

std::unique_ptr<AudioSystem> AudoutAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<AudoutAudioSystem>(function_dispatcher);
}

AudoutAudioSystem::AudoutAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

AudoutAudioSystem::~AudoutAudioSystem() = default;

X_STATUS AudoutAudioSystem::CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                                         AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new AudoutAudioDriver(memory(), semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void AudoutAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto audout_driver = dynamic_cast<AudoutAudioDriver*>(driver);
  assert_not_null(audout_driver);
  audout_driver->Shutdown();
  delete audout_driver;
}

}  // namespace rex::audio::audout

#endif  // REX_PLATFORM_SWITCH
