#pragma once
/**
 * @file        rex/ui/surface_switch.h
 * @brief       Presentation target backed by a libnx NWindow.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/ui/surface.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

namespace rex {
namespace ui {

// Consumed by the Vulkan presenter to build a VkSurfaceKHR through
// VK_NN_vi_surface - see VulkanPresenter::ConnectPaintingToSurfaceFromUIThread.
//
// Does not own the window. There is only ever one on this platform, it belongs
// to libnx, and it outlives everything that draws to it.
class ViWindowSurface final : public Surface {
 public:
  explicit ViWindowSurface(NWindow* window) : window_(window) {}

  TypeIndex GetType() const override { return kTypeIndex_NintendoViWindow; }

  NWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  NWindow* window_;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
