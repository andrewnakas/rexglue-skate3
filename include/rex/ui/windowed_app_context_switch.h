#pragma once
/**
 * @file        rex/ui/windowed_app_context_switch.h
 * @brief       The applet loop that owns the UI thread on Horizon.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>

#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace ui {

class SwitchWindow;

class SwitchWindowedAppContext final : public WindowedAppContext {
 public:
  SwitchWindowedAppContext() = default;
  ~SwitchWindowedAppContext() override;

  bool Initialize();

  // Runs until the system asks the application to exit or something calls
  // QuitFromUIThread. Returns the process exit code.
  int RunMainLoop();

  // The one window, registered while it is open so the loop can paint it and
  // hand it the applet transitions.
  void SetWindow(SwitchWindow* window) { window_ = window; }
  SwitchWindow* window() const { return window_; }

  // Woken by RequestPaintImpl and by anything queueing a pending function.
  void Wake() { ueventSignal(&wake_event_); }

  // The pad has to be sampled from the thread that owns the applet loop, but
  // the input driver lives in a library that links against this one rather than
  // the other way round. So the driver leaves a function here and the loop
  // calls it, which keeps the dependency pointing the way the build expects.
  using InputPumpCallback = void (*)();
  static void SetInputPump(InputPumpCallback pump);

 protected:
  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

 private:
  UEvent wake_event_{};
  AppletHookCookie applet_hook_cookie_{};
  SwitchWindow* window_ = nullptr;
  std::atomic<bool> quit_requested_{false};
  bool initialized_ = false;

  static void AppletHook(AppletHookType hook, void* param);
};

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
