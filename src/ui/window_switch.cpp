/**
 * @file        ui/window_switch.cpp
 * @brief       The single full-screen window on Horizon.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/ui/window_switch.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface_switch.h>
#include <rex/ui/windowed_app_context_switch.h>

REXCVAR_DEFINE_BOOL(
    switch_docked_1080p, false, "UI",
    "Render at 1920x1080 when the console is docked, instead of rendering 720p "
    "and letting the display scale it. Two and a quarter times the pixels for a "
    "GPU that is already the second thing holding the frame back, so this is off "
    "until the frame time says it can be afforded.");

namespace rex {
namespace ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<SwitchWindow>(app_context, title, desired_logical_width,
                                        desired_logical_height);
}

SwitchWindow::SwitchWindow(WindowedAppContext& app_context, const std::string_view title,
                           uint32_t desired_logical_width, uint32_t desired_logical_height)
    : super(app_context, title, desired_logical_width, desired_logical_height) {}

SwitchWindow::~SwitchWindow() {
  EnterDestructor();
  switch_app_context().SetWindow(nullptr);
}

SwitchWindowedAppContext& SwitchWindow::switch_app_context() const {
  return static_cast<SwitchWindowedAppContext&>(app_context());
}

void SwitchWindow::QueryOperationModeSize(uint32_t& width_out, uint32_t& height_out) const {
  // Handheld scans out 720p and nothing else. Docked can do 1080p, but only
  // because the display accepts it - the GPU still has to draw it.
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  if (docked && REXCVAR_GET(switch_docked_1080p)) {
    width_out = 1920;
    height_out = 1080;
  } else {
    width_out = 1280;
    height_out = 720;
  }
}

bool SwitchWindow::OpenImpl() {
  // libnx owns this; it is created before main and outlives everything here.
  window_ = nwindowGetDefault();
  if (!window_) {
    REXLOG_ERROR("SwitchWindow: no default NWindow");
    return false;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  QueryOperationModeSize(width, height);
  // This is the size of the buffers the compositor will scan out, not a request
  // the window manager may refuse: there is no window manager.
  if (R_FAILED(nwindowSetDimensions(window_, width, height))) {
    REXLOG_WARN("SwitchWindow: could not set the window to {}x{}; keeping what it has", width,
                height);
  }

  switch_app_context().SetWindow(this);

  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) {
    return true;
  }
  // An application is in focus the moment it is running; the applet hook
  // corrects this when the player presses Home.
  OnFocusUpdate(true, destruction_receiver);
  return true;
}

void SwitchWindow::RequestCloseImpl() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (!destruction_receiver.IsWindowDestroyed()) {
    switch_app_context().SetWindow(nullptr);
    // Deliberately not destroyed: the default NWindow belongs to libnx, and
    // tearing it down here would take the console's display with it.
    window_ = nullptr;
    OnAfterClose();
  }
}

std::unique_ptr<Surface> SwitchWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_NintendoViWindow)) {
    return nullptr;
  }
  if (!window_) {
    return nullptr;
  }
  return std::make_unique<ViWindowSurface>(window_);
}

void SwitchWindow::RequestPaintImpl() {
  // Coalesced: many requests between two loop iterations are one paint. The
  // loop clears the flag with TakePaintRequest.
  bool expected = false;
  if (!paint_requested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
    return;
  }
  switch_app_context().Wake();
}

uint32_t SwitchWindow::GetLatestDpiImpl() const {
  // There is one panel and one television, and neither reports a DPI. The
  // medium value keeps the settings overlay at its intended size; the mobile
  // scale factor on top of it is what actually makes it finger-sized.
  return GetMediumDpi();
}

float SwitchWindow::QueryDisplayRefreshHzImpl() const {
  // 60 Hz handheld and docked alike. The original hardware has no variable
  // refresh mode for an application to discover.
  return 60.0f;
}

bool SwitchWindow::PaintIfRequested() {
  if (!paint_requested_.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  // Blocks on the display when vsync is on, which is what paces the loop.
  OnPaint();
  return true;
}

void SwitchWindow::OnFocusStateChanged(bool has_focus) {
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(has_focus, destruction_receiver);
}

void SwitchWindow::OnOperationModeChanged() {
  if (!window_) {
    return;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  QueryOperationModeSize(width, height);
  if (R_FAILED(nwindowSetDimensions(window_, width, height))) {
    return;
  }
  REXLOG_INFO("SwitchWindow: now {} at {}x{}",
              appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
              width, height);

  WindowDestructionReceiver destruction_receiver(this);
  if (!OnActualSizeUpdate(width, height, destruction_receiver) ||
      destruction_receiver.IsWindowDestroyedOrClosed()) {
    return;
  }
  // The swapchain's images are the old size and the surface it was built from
  // describes the old window. Both have to be rebuilt before the next paint.
  OnSurfaceChanged(true);
}

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
