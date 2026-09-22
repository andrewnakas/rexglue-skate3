#pragma once

#include <atomic>
#include <functional>

#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL_events.h>

namespace rex {
namespace ui {

class SDLWindowedAppContext final : public WindowedAppContext {
 public:
  SDLWindowedAppContext() = default;
  ~SDLWindowedAppContext() override = default;

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  int RunMainLoop();

  // Called on the UI thread when the system reports memory pressure. Whatever
  // the app can rebuild on demand should be released here; ignoring the
  // warning means the system reclaims resources itself, underneath the
  // graphics driver.
  void SetLowMemoryHandler(std::function<void()> handler) {
    low_memory_handler_ = std::move(handler);
  }

  // Called from the lifecycle watch as the app suspends and resumes, to stop
  // and restart the work that has no business continuing in the background -
  // the guest threads, the command processor and the audio pump.
  //
  // Separate from the low-memory handler because they answer different
  // questions (what can be rebuilt, versus what should not be running) and
  // because the suspend one must have an exact counterpart on resume, which
  // releasing caches does not.
  void SetSuspendHandlers(std::function<void()> on_suspend, std::function<void()> on_resume) {
    suspend_handler_ = std::move(on_suspend);
    resume_handler_ = std::move(on_resume);
  }

 private:
  void DispatchEvent(const SDL_Event& event);

  std::atomic_bool pending_functions_event_queued_ = false;
  std::function<void()> low_memory_handler_;
  std::function<void()> suspend_handler_;
  std::function<void()> resume_handler_;
};

}  // namespace ui
}  // namespace rex
