#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/platform.h>
#include <rex/ui/surface.h>

#if REX_PLATFORM_ANDROID

#include <android/native_window.h>

namespace rex {
namespace ui {

// Presentation target backed by an ANativeWindow, as obtained from a
// SurfaceHolder / NativeActivity. Consumed by the Vulkan presenter to build a
// VkAndroidSurfaceKHR - see VulkanPresenter::ConnectPaintingToSurfaceFromUIThread.
//
// Does not own the window: the Android framework destroys it on
// surfaceDestroyed, which must happen only after painting has been
// disconnected.
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}

  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }

  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
