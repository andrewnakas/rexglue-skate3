#include <rex/ui/windowed_app_context_sdl.h>
#include <rex/ui/window_sdl.h>

#include <rex/logging.h>

#include <cstdlib>

namespace rex {
namespace ui {

namespace {

constexpr uint32_t kPendingFunctionsEvent = SDL_EVENT_USER;

}  // namespace

void SDLWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  bool expected = false;
  if (!pending_functions_event_queued_.compare_exchange_strong(expected, true)) {
    return;
  }

  SDL_Event event = {};
  event.type = kPendingFunctionsEvent;
  if (!SDL_PushEvent(&event)) {
    pending_functions_event_queued_ = false;
  }
}

void SDLWindowedAppContext::PlatformQuitFromUIThread() {
  SDL_Event event = {};
  event.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&event);
}

int SDLWindowedAppContext::RunMainLoop() {
  if (HasQuitFromUIThread()) {
    return EXIT_SUCCESS;
  }

  SDL_Event event;
  while (!HasQuitFromUIThread() && SDL_WaitEvent(&event)) {
    DispatchEvent(event);
  }

  if (!HasQuitFromUIThread()) {
    REXLOG_WARN("SDL event loop exited unexpectedly: {}", SDL_GetError());
    QuitFromUIThread();
  }
  return EXIT_SUCCESS;
}

void SDLWindowedAppContext::DispatchEvent(const SDL_Event& event) {
  if (event.type == kPendingFunctionsEvent) {
    pending_functions_event_queued_ = false;
    ExecutePendingFunctionsFromUIThread();
    return;
  }

  if (event.type == SDL_EVENT_QUIT) {
    REXLOG_INFO("SDL quit event received");
    QuitFromUIThread();
    return;
  }

  // iOS asks before it takes. Something else on the device wanting memory - a
  // phone call is the usual one - makes the system send this, and an app that
  // ignores it gets its resources reclaimed underneath it instead, which
  // surfaces as a fault somewhere in the graphics driver rather than as
  // anything recognisable. Handing back the caches is far cheaper than being
  // reclaimed: they rebuild from guest memory on demand.
  if (event.type == SDL_EVENT_LOW_MEMORY) {
    REXLOG_WARN("system reported low memory; releasing caches");
    if (low_memory_handler_) {
      low_memory_handler_();
    }
    return;
  }

  // Going to the background is the moment that actually matters on iOS, and
  // the low-memory warning above is not it: that warning has never once fired
  // on device across every session ever captured, while being killed while
  // suspended happens constantly - the app comes back as a fresh launch
  // instead of where the player left off.
  //
  // Jetsam evicts suspended processes largest-first, and this one sits near a
  // gigabyte, most of it caches. Handing them back before suspending drops it
  // by hundreds of megabytes and makes the process a far less attractive
  // target. The cost on return is a few seconds of redecoding from guest
  // memory, which is what these caches are for.
  //
  // Deliberately memory only. Suspending PRESENTATION here is a separate and
  // much riskier change - it was tried and reverted for freezing on return -
  // so this does not touch it.
  if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
    REXLOG_INFO("entering the background; releasing caches so the app is not evicted");
    if (low_memory_handler_) {
      low_memory_handler_();
    }
    return;
  }

  SDLWindow::HandleSDLEvent(event);
}

}  // namespace ui
}  // namespace rex
