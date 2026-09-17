/**
 * @file        ui/windowed_app_context_switch.cpp
 * @brief       The applet loop that owns the UI thread on Horizon.
 *
 * Every platform in this engine has some loop that belongs to the window
 * system; here it is libnx's applet loop, which is also how the system tells an
 * application that the player pressed Home, docked the console, or asked it to
 * close.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/ui/windowed_app_context_switch.h>

#include <rex/logging.h>
#include <rex/ui/window_switch.h>

namespace rex {
namespace ui {

namespace {

SwitchWindowedAppContext::InputPumpCallback input_pump_ = nullptr;

// How long the loop sleeps when it has nothing to paint. Short enough that a
// pending function queued from another thread is picked up promptly, long
// enough that an idle loop is not a spin. Painting itself blocks on the
// display, so this only runs when there is genuinely no frame to produce.
constexpr u64 kIdleWaitNanoseconds = 1'000'000;  // 1 ms

}  // namespace

void SwitchWindowedAppContext::SetInputPump(InputPumpCallback pump) {
  input_pump_ = pump;
}

SwitchWindowedAppContext::~SwitchWindowedAppContext() {
  if (initialized_) {
    appletUnhook(&applet_hook_cookie_);
    initialized_ = false;
  }
}

bool SwitchWindowedAppContext::Initialize() {
  ueventCreate(&wake_event_, /*auto_clear=*/true);
  appletHook(&applet_hook_cookie_, &SwitchWindowedAppContext::AppletHook, this);
  initialized_ = true;
  return true;
}

void SwitchWindowedAppContext::AppletHook(AppletHookType hook, void* param) {
  auto* context = static_cast<SwitchWindowedAppContext*>(param);
  if (!context) {
    return;
  }
  switch (hook) {
    case AppletHookType_OnFocusState: {
      // Losing focus means the player opened the Home menu or the console went
      // to sleep. The compositor stops handing out buffers, so a paint would
      // block; the window's focus state is what stops it being attempted.
      const bool has_focus = appletGetFocusState() == AppletFocusState_InFocus;
      REXLOG_INFO("SwitchWindowedAppContext: focus {}", has_focus ? "gained" : "lost");
      if (context->window_) {
        context->window_->OnFocusStateChanged(has_focus);
      }
    } break;

    case AppletHookType_OnOperationMode:
      // Docked or undocked. The display resolution changes underneath the
      // window, so the swapchain has to be rebuilt.
      if (context->window_) {
        context->window_->OnOperationModeChanged();
      }
      break;

    case AppletHookType_OnExitRequest:
      // The system is asking, not telling: there is a short grace period before
      // it stops being a request. Quit through the ordinary path so the guest
      // gets to shut down and settings are written.
      REXLOG_INFO("SwitchWindowedAppContext: the system asked the application to exit");
      context->quit_requested_.store(true, std::memory_order_release);
      context->Wake();
      break;

    default:
      break;
  }
}

void SwitchWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  Wake();
}

void SwitchWindowedAppContext::PlatformQuitFromUIThread() {
  quit_requested_.store(true, std::memory_order_release);
  Wake();
}

int SwitchWindowedAppContext::RunMainLoop() {
  Waiter wake_waiter = waiterForUEvent(&wake_event_);

  while (appletMainLoop()) {
    if (quit_requested_.load(std::memory_order_acquire) || HasQuitFromUIThread()) {
      break;
    }

    ExecutePendingFunctionsFromUIThread();

    // Sampled once per iteration, on this thread, because that is where hid
    // wants to be read from and where the settings overlay reads it.
    if (input_pump_) {
      input_pump_();
    }

    if (window_ && window_->PaintIfRequested()) {
      continue;
    }

    // Nothing to draw: wait to be woken rather than spinning. On three cores a
    // spinning UI thread is a third of the machine.
    s32 index = 0;
    waitObjects(&index, &wake_waiter, 1, kIdleWaitNanoseconds);
  }

  // Anything still queued must run: a caller may be blocked on a fence that one
  // of these pending functions signals.
  ExecutePendingFunctionsFromUIThread();
  return 0;
}

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
