#pragma once
/**
 * @file        rex/ui/window_switch.h
 * @brief       The single full-screen window on Horizon.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <string_view>

#include <rex/ui/window.h>

namespace rex {
namespace ui {

class SwitchWindowedAppContext;

// There is exactly one of these, it is always full-screen, and the player
// cannot move, resize or close it. What does change underneath it is the
// resolution: docking the console switches the display from 720p to 1080p, and
// the window has to be told, which is what OnOperationModeChanged is for.
class SwitchWindow final : public Window {
  using super = Window;

 public:
  SwitchWindow(WindowedAppContext& app_context, const std::string_view title,
               uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~SwitchWindow() override;

  // Called by the applet loop once per iteration. Consumes a pending paint
  // request and paints, returning whether it did. The painting itself stays
  // inside the window, which is where the base class keeps it.
  bool PaintIfRequested();
  void OnFocusStateChanged(bool has_focus);
  void OnOperationModeChanged();

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;
  uint32_t GetLatestDpiImpl() const override;
  float QueryDisplayRefreshHzImpl() const override;

 private:
  // The resolution the display is actually scanning out, which follows the dock
  // state rather than anything the app asks for.
  void QueryOperationModeSize(uint32_t& width_out, uint32_t& height_out) const;

  SwitchWindowedAppContext& switch_app_context() const;

  NWindow* window_ = nullptr;
  std::atomic<bool> paint_requested_{false};
};

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
