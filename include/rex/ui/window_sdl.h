#pragma once

#include <memory>
#include <string>
#include <atomic>

#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>

#include <SDL3/SDL_events.h>
#if REX_PLATFORM_MAC
#include <SDL3/SDL_metal.h>
#endif
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

namespace rex {
namespace ui {

class SDLWindow final : public Window {
  using super = Window;

 public:
  SDLWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~SDLWindow() override;

  static void HandleSDLEvent(const SDL_Event& event);

  // Marks every open window's surface presentable or not - see
  // Window::IsSurfacePresentable(). Static because the iOS lifecycle watch in
  // SDLWindowedAppContext has no window in hand: it runs synchronously from
  // the UIKit delegate, before any per-window event is dispatched, which is
  // the only point early enough to stop a paint that would otherwise block on
  // a drawable the system will not hand out while suspended.
  //
  // UI thread only (both SDL window events and the lifecycle watch run there),
  // so the window map needs no lock of its own.
  static void SetAllSurfacesPresentable(bool presentable, const char* reason);

#if REX_PLATFORM_ANDROID
  // Android destroys the ANativeWindow behind the SDL window whenever the
  // activity leaves the foreground and hands out a NEW one on return. The
  // presenter's VkSurfaceKHR is bound to the old pointer, so every open window
  // drops its surface before the old window dies and creates a fresh one once
  // SDL has stored the new pointer. Both go through Window::OnSurfaceChanged,
  // the mechanism Xenia's own Android window used for exactly this. UI thread
  // only, like SetAllSurfacesPresentable, and for the same reason: they are
  // called from the lifecycle event watch.
  static void DetachAllSurfaces(const char* reason);
  static void ReattachAllSurfaces(const char* reason);
#endif

  void SetTextInputActive(bool active) override;

  // Recomputes this window's surface presentability from SDL_GetWindowFlags.
  // Returns whether it changed. See the definition for why the flags are read
  // instead of trusting SDL_EVENT_WINDOW_EXPOSED.
  bool RefreshSurfacePresentableFromWindowFlags();

 protected:
  uint32_t GetLatestDpiImpl() const override;
  float QueryDisplayRefreshHzImpl() const override;
  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void HandleEvent(const SDL_Event& event);
  void HandleSizeUpdate(WindowDestructionReceiver& destruction_receiver);
  void HandleKey(const SDL_KeyboardEvent& event, bool down,
                 WindowDestructionReceiver& destruction_receiver);
  void HandleMouseButton(const SDL_MouseButtonEvent& event, bool down,
                         WindowDestructionReceiver& destruction_receiver);
  void HandleMouseMotion(const SDL_MouseMotionEvent& event,
                         WindowDestructionReceiver& destruction_receiver);
  void HandleMouseWheel(const SDL_MouseWheelEvent& event,
                        WindowDestructionReceiver& destruction_receiver);
  void HandleTextInput(const SDL_TextInputEvent& event,
                       WindowDestructionReceiver& destruction_receiver);
  void RevealAutoHiddenCursor();
  void SetCursorAutoHideTimer();
  void RemoveCursorAutoHideTimer();
  void WindowPointToPhysical(float x, float y, int32_t& physical_x, int32_t& physical_y) const;
  uint32_t QueryDpi() const;

  SDL_Window* window_ = nullptr;
#if REX_PLATFORM_MAC
  SDL_MetalView metal_view_ = nullptr;
  void* metal_layer_ = nullptr;
#endif
  std::atomic<bool> paint_event_queued_{false};
  SDL_TimerID cursor_auto_hide_timer_ = 0;
  bool cursor_currently_auto_hidden_ = false;
  uint32_t dpi_ = 96;
};

class SDLMenuItem final : public MenuItem {
 public:
  SDLMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback)
      : MenuItem(type, text, hotkey, std::move(callback)) {}
};

}  // namespace ui
}  // namespace rex
