/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/input/input_driver.h>

#include <SDL3/SDL.h>

#define HID_SDL_USER_COUNT 4
#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT GetStateUi(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;
  void OnSystemResume() override;

 private:
  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    // The right stick as the PHYSICAL stick last reported it. While a finger
    // is on the right touchpad the pad owns thumb_r*, so the stick's own value
    // is parked here and put back on release - otherwise letting go of the pad
    // would leave the stick reading centre until it was next moved.
    int16_t stick_rx;
    int16_t stick_ry;
    bool touchpad_active;
    // Which finger owns the stick. A second finger landing must not steal it,
    // and that finger lifting must not recentre it.
    int32_t touchpad_finger;
    // Where the finger is (target) and where the stick has actually got to
    // (current), both -1..1. The gap between them is the smoothing: the stick
    // travels to the finger instead of teleporting, which is what gives the
    // game a flick to read rather than a jump.
    float pad_target_x;
    float pad_target_y;
    float pad_current_x;
    float pad_current_y;
    // Set when the finger lifts. The stick does not coast home from there: it
    // is pinned at the lift position for a beat so the game cannot miss the
    // end of the flick between two polls, and then snapped straight back to
    // the physical stick. Coasting was what put the board into a manual -
    // a stick easing back over a hundred milliseconds is a stick being HELD,
    // and held-down is exactly the manual input.
    bool touchpad_releasing;
    uint64_t touchpad_last_ns;
    uint64_t touchpad_release_ns;
    // Where the finger was when it lifted, and when it last actually MOVED.
    //
    // The Steam Deck's pad reports lifts that never happened: a finger held
    // still on it produces an UP immediately followed by a DOWN in the same
    // place, sometimes in the same millisecond. Measured on hardware, 29 of 74
    // lifts in one session were phantoms like this. Left alone each one either
    // completes a release or re-acquires and resets the stick to centre, so a
    // held preload keeps collapsing under a thumb that never moved.
    //
    // These let a re-touch be recognised as the same contact continuing, and
    // let a lift from a STATIONARY finger wait a little longer before being
    // believed - while a lift from a moving one, which is a real flick, is
    // still acted on at once.
    float touchpad_lift_x;
    float touchpad_lift_y;
    uint64_t touchpad_last_move_ns;
    bool touchpad_lift_was_stationary;
    // Left pad acting as a d-pad. Press-activated, so this holds the direction
    // bits currently asserted and the finger that is pressing.
    uint16_t left_pad_buttons;
    bool left_pad_active;
    int32_t left_pad_finger;
  };

  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void StopRumbleLocked(ControllerState& state);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerTouchpadLocked(const SDL_Event& event);
  void OnRightTouchpadLocked(const SDL_Event& event, ControllerState& controller);
  void OnLeftTouchpadLocked(const SDL_Event& event, ControllerState& controller);
  // Advance every touchpad-driven stick toward its target. Called from the
  // state poll, because that is the only thing that ticks per frame.
  void AdvanceTouchpadSmoothingLocked();
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* GetControllerState(uint32_t user_index);
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  std::array<ControllerState, HID_SDL_USER_COUNT> controllers_;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;
};

}  // namespace rex::input::sdl
