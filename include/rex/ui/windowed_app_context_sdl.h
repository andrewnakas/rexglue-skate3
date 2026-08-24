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

 private:
  void DispatchEvent(const SDL_Event& event);

  std::atomic_bool pending_functions_event_queued_ = false;
  std::function<void()> low_memory_handler_;
};

}  // namespace ui
}  // namespace rex
