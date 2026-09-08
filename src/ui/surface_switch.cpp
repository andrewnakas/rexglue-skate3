/**
 * @file        ui/surface_switch.cpp
 * @brief       Presentation target backed by a libnx NWindow.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/ui/surface_switch.h>

namespace rex {
namespace ui {

bool ViWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  if (!window_) {
    width_out = 0;
    height_out = 0;
    return false;
  }
  u32 width = 0;
  u32 height = 0;
  if (R_FAILED(nwindowGetDimensions(window_, &width, &height)) || !width || !height) {
    // A zero size means "not ready to present yet" to the caller, which is the
    // right reading during a dock or undock transition: the window is being
    // resized and the swapchain should wait rather than fail.
    width_out = 0;
    height_out = 0;
    return false;
  }
  width_out = width;
  height_out = height;
  return true;
}

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
