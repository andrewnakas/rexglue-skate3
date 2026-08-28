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

  // iOS lifecycle events must be handled from an event WATCH, not from the
  // loop below. UIKit suspends the process as soon as its delegate returns, so
  // an event that is merely queued for SDL_WaitEvent is not seen until the app
  // is already coming back - far too late to act on. SDL dispatches watches
  // synchronously from the delegate, which is the only point where releasing
  // memory still changes whether the system evicts us.
  //
  // This is why the low-memory handler appeared never to fire: the event was
  // being queued and arriving too late to matter, not going missing.
  SDL_AddEventWatch(
      [](void* userdata, SDL_Event* event) -> bool {
        auto* context = static_cast<SDLWindowedAppContext*>(userdata);
        switch (event->type) {
          case SDL_EVENT_WILL_ENTER_BACKGROUND:
            // Stop painting BEFORE the process suspends. A backgrounded
            // CAMetalLayer does not vend drawables, and MoltenVK takes the
            // drawable inside vkQueueSubmit on an untimed wait, so a paint
            // started now does not fail - it blocks, holding the queue lock,
            // and the app never goes quiescent. iOS then kills it for failing
            // to suspend, which writes no crash report.
            //
            // Every device-lost in the captured logs followed one of these
            // events by ~1s (the drawable timeout), and every one of them was
            // the last line its process ever wrote. This is that fix, and it
            // has to happen here in the watch rather than in DispatchEvent -
            // by the time a queued event is dispatched the app is already
            // suspended.
            SDLWindow::SetAllSurfacesPresentable(false, "entering the background");
            [[fallthrough]];
          case SDL_EVENT_LOW_MEMORY:
            // Jetsam evicts suspended processes largest-first, and this one
            // sits near a gigabyte, most of it caches it cannot use while
            // suspended. Handing them back here is the difference between
            // resuming and relaunching. They refill from guest memory.
            REXLOG_INFO("app lifecycle: releasing caches before suspending (event {})",
                        uint32_t(event->type));
            if (context->low_memory_handler_) {
              context->low_memory_handler_();
            }
            break;
          // BOTH foreground events, because WILL_ENTER_FOREGROUND is not
          // reliably delivered - measured on device, the app logged 10
          // suspends against 5 resumes and every resume came from the window
          // flags path, never from this one. An unmatched suspend leaves the
          // gate shut, the app stops painting for good, and it looks exactly
          // like the hang this gate exists to prevent. Handling both, and
          // being idempotent, costs nothing.
          case SDL_EVENT_WILL_ENTER_FOREGROUND:
          case SDL_EVENT_DID_ENTER_FOREGROUND:
            SDLWindow::SetAllSurfacesPresentable(true, "returning to the foreground");
            break;
          default:
            break;
        }
        // Watches observe; the event still goes to the queue.
        return true;
      },
      this);

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
