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

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");
REXCVAR_DEFINE_UINT32(hid_sdl_rumble_duration_ms, 100, "Input",
                      "SDL rumble pulse duration for nonzero XInput vibration");
REXCVAR_DEFINE_BOOL(hid_sdl_touchpad_right_stick, true, "Input",
                    "Drive the right thumbstick from the controller's right touchpad, as an "
                    "absolute position: where your finger is on the pad is where the stick is. "
                    "Lifting off returns the stick to the physical stick's own position. Needs a "
                    "pad that reports touchpads to SDL - on a Steam Deck that means Steam Input "
                    "must not be consuming them for the game's own layout.");
REXCVAR_DEFINE_INT32(hid_sdl_touchpad_right_index, 1, "Input",
                     "Which SDL touchpad drives the right stick. On a Steam Deck 0 is the left "
                     "pad and 1 the right. -1 turns it off.");
REXCVAR_DEFINE_DOUBLE(hid_sdl_touchpad_smoothing_ms, 30.0, "Input",
                      "How long the right-stick touchpad takes to catch up with your finger, as "
                      "a time constant in milliseconds. The stick starts at CENTRE on each new "
                      "touch and travels out, so the game sees a continuous flick instead of the "
                      "stick teleporting to full deflection - which is what a flip trick needs. "
                      "Lower is snappier and twitchier, higher is smoother and laggier. 0 "
                      "restores the old instant jump.")
    .range(0.0, 500.0);
REXCVAR_DEFINE_DOUBLE(hid_sdl_touchpad_full_deflection, 0.7, "Input",
                      "How far out from the centre of the pad, 0..1, counts as the stick being "
                      "all the way over. Everything past it is full deflection in that "
                      "direction. It is deliberately well inside the pad: a thumb on the bottom "
                      "edge has to read as a HARD stick-down for a big ollie, and it cannot do "
                      "that if the edge of the pad is only just barely full. The trade is a "
                      "smaller region in which partial deflection is expressible.")
    .range(0.25, 1.0);
REXCVAR_DEFINE_DOUBLE(
    hid_sdl_touchpad_reacquire_ms, 120.0, "Input",
    "How long, in milliseconds, a lift from a STATIONARY thumb is doubted before it is "
    "believed. The Steam Deck's pad reports lifts that never happened - a finger held still "
    "produces an UP and then a DOWN in the same place, sometimes in the same millisecond - and "
    "each one collapses a held preload. Within this window a re-touch near where the thumb "
    "supposedly left is treated as the same contact continuing, and the stick keeps its "
    "position instead of being yanked to centre. A lift from a MOVING thumb is a real flick and "
    "is acted on immediately whatever this is set to, so raising it does not make tricks less "
    "responsive - it only delays the pop at the end of a deliberate, motionless preload. 0 "
    "restores the old behaviour of believing every lift at once.")
    .range(0.0, 400.0);
REXCVAR_DEFINE_BOOL(hid_sdl_touchpad_trace, false, "Input",
                    "Log every right-touchpad event and the resulting stick value, for working "
                    "out why a gesture does not read the way it feels. Noisy by design.");
REXCVAR_DEFINE_DOUBLE(hid_sdl_touchpad_flick_hold_ms, 32.0, "Input",
                      "How long the stick stays pinned where your thumb left it, in "
                      "milliseconds, before it snaps back to centre. It exists so a fast flick "
                      "cannot land entirely between two input polls and be missed. It must stay "
                      "SHORT: hold it much longer and the game reads a held stick, which is a "
                      "manual, not a flick.")
    .range(0.0, 250.0);
REXCVAR_DEFINE_BOOL(hid_sdl_touchpad_invert_y, false, "Input",
                    "Flip up and down on both touchpads. SDL reports the Steam Deck's pads with "
                    "Y increasing upward, which already matches a thumbstick, so this is off. "
                    "Turn it on if up and down come out backwards - ollie where you wanted "
                    "nollie, d-pad up where you pressed down - on some other controller.");
REXCVAR_DEFINE_BOOL(hid_sdl_touchpad_invert_x, false, "Input",
                    "Flip left and right on both touchpads. The counterpart to "
                    "hid_sdl_touchpad_invert_y, and off for the same reason.");
REXCVAR_DEFINE_BOOL(hid_sdl_touchpad_left_dpad, true, "Input",
                    "Drive the d-pad from the left touchpad. PRESS-activated: resting a thumb "
                    "does nothing, pressing the pad down sends the direction under your thumb. "
                    "That is deliberate - a touch-activated d-pad fires constantly while your "
                    "hand rests there.");
REXCVAR_DEFINE_INT32(hid_sdl_touchpad_left_index, 0, "Input",
                     "Which SDL touchpad drives the d-pad. On a Steam Deck 0 is the left pad. "
                     "-1 turns it off.");
REXCVAR_DEFINE_DOUBLE(hid_sdl_touchpad_press_threshold, 0.5, "Input",
                      "How hard the left pad must be pressed to count as a press, 0..1 of the "
                      "pressure SDL reports. Lower it if pressing does nothing; set it to 0 to "
                      "make the pad touch-activated instead.")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(hid_sdl_touchpad_dpad_deadzone, 0.35, "Input",
                      "How far from the centre of the left pad a press has to be before it "
                      "counts as a direction, 0..1. Presses inside this do nothing, so the "
                      "middle of the pad is not a coin-flip between two directions.")
    .range(0.0, 0.95);

namespace rex::input::sdl {

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_(),
      keystroke_states_() {}

SDLInputDriver::~SDLInputDriver() {}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Match Xenia's SDL setup: keep SDL from calling timeBeginPeriod(1) and
      // weakening the process-wide NT timer resolution requested on startup.
      SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      // Load custom controller mappings if available. A relative path is
      // resolved against the application root rather than the working
      // directory, which is undefined when launched from a bundle/Finder.
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (mappings_path.is_relative()) {
          mappings_path = rex::filesystem::GetAppRootFolder() / mappings_path;
        }
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      mappings_path.string());
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(mappings_path.string().c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         mappings_path.string(), mappings_result);
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    for (size_t i = 0; i < controllers_.size(); i++) {
      if (controllers_.at(i).sdl) {
        StopRumbleLocked(controllers_.at(i));
        SDL_CloseGamepad(controllers_.at(i).sdl);
        controllers_.at(i) = {};
      }
    }
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  // The touchpad stick is steered here rather than from the events: SDL only
  // sends touchpad motion when the finger MOVES, and a finger held still at
  // the edge of the pad still has to be carried there smoothly.
  AdvanceTouchpadSmoothingLocked();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetStateUi(uint32_t user_index, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Raw state, no is_active gating, and no packet/state_changed bookkeeping -
  // that belongs to the guest-facing GetState path.
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const bool has_rumble = vibration->left_motor_speed || vibration->right_motor_speed;
  const uint32_t duration_ms =
      has_rumble ? std::clamp(REXCVAR_GET(hid_sdl_rumble_duration_ms), uint32_t(1),
                              uint32_t(1000))
                 : 0;
  if (!SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                         vibration->right_motor_speed, duration_ms)) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  bool user_any = users == 0xFF;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    if (!controller) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    const uint64_t curr_butts =
        is_active
            ? (controller->state.gamepad.buttons | AnalogToKeyfield(controller->state.gamepad))
            : uint64_t(0);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != rex::ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2; clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        rex::ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == rex::ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
      OnControllerTouchpadLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::StopRumbleLocked(ControllerState& state) {
  if (state.sdl) {
    SDL_RumbleGamepad(state.sdl, 0, 0, 0);
  }
}

void SDLInputDriver::OnSystemResume() {
  // Close every gamepad and open whatever is actually there now.
  //
  // SDL does not reliably re-announce a device that went away and came back
  // while the machine was asleep - and on the Deck the pad is USB, so it does
  // exactly that. Without this the driver keeps a handle that no longer refers
  // to anything, every poll returns the last state it ever saw, and the game
  // sits there being told the sticks are centred and no buttons are down. The
  // picture stops moving, which is indistinguishable from the engine having
  // frozen unless you go looking - and sixteen builds went looking in the
  // wrong place.
  auto lock = DrainAndLock();

  size_t closed = 0;
  for (size_t i = 0; i < controllers_.size(); ++i) {
    auto& state = controllers_.at(i);
    if (!state.sdl) {
      continue;
    }
    StopRumbleLocked(state);
    SDL_CloseGamepad(state.sdl);
    state = {};
    keystroke_states_.at(i) = {};
    ++closed;
  }

  // SDL's own view of the devices can be stale too: make it re-enumerate
  // before asking it what exists.
  SDL_UpdateGamepads();
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  size_t opened = 0;
  if (ids) {
    for (int i = 0; i < count; ++i) {
      // Reuse the add path so slot assignment, capabilities and the initial
      // packet number are all established exactly as they are at startup.
      SDL_Event added = {};
      added.type = SDL_EVENT_GAMEPAD_ADDED;
      added.cdevice.which = ids[i];
      OnControllerDeviceAddedLocked(added);
      ++opened;
    }
    SDL_free(ids);
  }
  REXLOG_INFO("SDL: woke from suspend - closed {} stale gamepad(s), re-opened {}", closed, opened);
  if (!opened) {
    REXLOG_ERROR(
        "SDL: woke from suspend and there is NO gamepad. Nothing will respond to the sticks or "
        "buttons until one appears.");
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  // Open the controller.
  const auto controller = SDL_OpenGamepad(event.cdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));
  int user_id = -1;
  // Check if the controller has a player index LED.
  user_id = SDL_GetGamepadPlayerIndex(controller);
  // Is that id already taken?
  if (user_id < 0 || user_id >= static_cast<int>(controllers_.size()) ||
      controllers_.at(user_id).sdl) {
    user_id = -1;
  }
  // No player index or already taken, just take the first free slot.
  if (user_id < 0) {
    for (size_t i = 0; i < controllers_.size(); i++) {
      if (!controllers_.at(i).sdl) {
        user_id = static_cast<int>(i);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetGamepadPlayerIndex(controller, user_id);
#endif
        break;
      }
    }
  }
  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    state = {controller, {}};
    // XInput seems to start with packet_number = 1 .
    state.state_changed = true;
    UpdateXCapabilities(state);

    REXLOG_INFO("SDL OnControllerDeviceAdded: Added at index {}.", user_id);
  } else {
    // No more controllers needed, close it.
    SDL_CloseGamepad(controller);
    REXLOG_WARN("SDL OnControllerDeviceAdded: Ignored. No free slots.");
  }
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  // Find the disconnected gamecontroller and close it.
  auto idx = GetControllerIndexFromInstanceID(event.cdevice.which);
  if (idx) {
    StopRumbleLocked(controllers_.at(*idx));
    SDL_CloseGamepad(controllers_.at(*idx).sdl);
    controllers_.at(*idx) = {};
    keystroke_states_.at(*idx) = {};
    REXLOG_INFO("SDL OnControllerDeviceRemoved: Removed at player index {}.", *idx);
  } else {
    // Can happen in case all slots where full previously.
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored. Unused device.");
  }
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  assert(idx);
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    // The touchpad, when a finger is on it, owns the right stick. Record what
    // the physical stick is doing either way so releasing the pad can hand it
    // straight back without waiting for the next stick movement.
    case SDL_GAMEPAD_AXIS_RIGHTX:
      controllers_.at(*idx).stick_rx = event.gaxis.value;
      if (!controllers_.at(*idx).touchpad_active) {
        pad.thumb_rx = event.gaxis.value;
      }
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      controllers_.at(*idx).stick_ry = static_cast<int16_t>(~event.gaxis.value);
      if (!controllers_.at(*idx).touchpad_active) {
        pad.thumb_ry = controllers_.at(*idx).stick_ry;
      }
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

namespace {

// -1..1 onto the XInput thumb range. Scaled by the POSITIVE maximum: the range
// is asymmetric (-32768..32767), so scaling by 32768 would send a full-left
// reading to -32768 and a full-right one past 32767 into a wrap.
int16_t FloatToThumb(float value) {
  const float scaled = std::clamp(value, -1.0f, 1.0f) * 32767.0f;
  return static_cast<int16_t>(std::lround(scaled));
}

// SDL reports a touchpad as 0..1 across each axis. Centre that onto -1..1 and
// apply the per-axis inversion cvars. Both pads go through here so that a
// controller reporting its pad the other way up is one setting to fix, not
// two, and so the d-pad and the stick can never disagree about which way is
// up.
float PadAxisX(float raw) {
  const float x = raw * 2.0f - 1.0f;
  return REXCVAR_GET(hid_sdl_touchpad_invert_x) ? -x : x;
}

float PadAxisY(float raw) {
  const float y = raw * 2.0f - 1.0f;
  return REXCVAR_GET(hid_sdl_touchpad_invert_y) ? -y : y;
}

}  // namespace

// Absolute touchpad -> right stick: the finger's position on the pad IS the
// stick's position, rather than the pad nudging a stick that stays where it
// was left.
void SDLInputDriver::OnControllerTouchpadLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gtouchpad.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);

  const int32_t right = REXCVAR_GET(hid_sdl_touchpad_right_index);
  if (REXCVAR_GET(hid_sdl_touchpad_right_stick) && right >= 0 &&
      event.gtouchpad.touchpad == right) {
    OnRightTouchpadLocked(event, controller);
  }
  const int32_t left = REXCVAR_GET(hid_sdl_touchpad_left_index);
  if (REXCVAR_GET(hid_sdl_touchpad_left_dpad) && left >= 0 && event.gtouchpad.touchpad == left) {
    OnLeftTouchpadLocked(event, controller);
  }
}

void SDLInputDriver::OnRightTouchpadLocked(const SDL_Event& event, ControllerState& controller) {
  auto& pad = controller.state.gamepad;

  if (REXCVAR_GET(hid_sdl_touchpad_trace)) {
    const char* kind = event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN   ? "DOWN"
                       : event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP   ? "UP  "
                                                                       : "MOVE";
    REXLOG_INFO(
        "rpad {}: finger={} raw=({:.3f},{:.3f}) pressure={:.3f} | active={} releasing={} "
        "owner={}",
        kind, event.gtouchpad.finger, event.gtouchpad.x, event.gtouchpad.y,
        event.gtouchpad.pressure, controller.touchpad_active ? 1 : 0,
        controller.touchpad_releasing ? 1 : 0, controller.touchpad_finger);
  }

  if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
    // Only the finger holding the stick can let go of it.
    if (!controller.touchpad_active || event.gtouchpad.finger != controller.touchpad_finger) {
      return;
    }
    // Snap the stick out to where the thumb actually was at the instant it
    // left the pad, rather than wherever the smoothing had got to. A flick is
    // over in a handful of milliseconds and the smoothed value is always
    // behind, so without this the game never sees the end of the gesture and
    // the trick that comes out is the one you were halfway through, not the
    // one you did. Then hold it there for flick_hold_ms and drop it dead.
    controller.pad_current_x = controller.pad_target_x;
    controller.pad_current_y = controller.pad_target_y;
    controller.touchpad_releasing = true;
    const uint64_t up_ns = rex::chrono::Clock::QueryHostSystemTime();
    controller.touchpad_release_ns = up_ns;
    controller.touchpad_lift_x = controller.pad_target_x;
    controller.touchpad_lift_y = controller.pad_target_y;
    // Was the thumb actually moving when the pad says it left?
    //
    // A flick is a fast gesture and is still travelling at the instant of the
    // lift. A preload is a thumb planted still at full deflection. The Deck's
    // pad emits phantom lifts under a STILL thumb, so that is the case worth
    // being sceptical about - and it is also the case where waiting a few more
    // milliseconds costs nothing, because nothing is moving.
    double since_move_ms = 1e9;
    if (controller.touchpad_last_move_ns != 0 && up_ns > controller.touchpad_last_move_ns) {
      since_move_ms = double(up_ns - controller.touchpad_last_move_ns) / 10000.0;
    }
    controller.touchpad_lift_was_stationary = since_move_ms >= 60.0;
    return;
  }

  if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
    // A re-touch that arrives almost immediately, in almost the same place, is
    // the pad having dropped and regained a finger that never left. Continue
    // the gesture instead of starting a new one: keep the stick exactly where
    // it is, rather than yanking it to centre and travelling back out. That
    // yank is what collapses a held preload under a motionless thumb.
    if (controller.touchpad_releasing &&
        event.gtouchpad.finger == controller.touchpad_finger) {
      const uint64_t now_ns = rex::chrono::Clock::QueryHostSystemTime();
      double gap_ms = 1e9;
      if (controller.touchpad_release_ns != 0 && now_ns > controller.touchpad_release_ns) {
        gap_ms = double(now_ns - controller.touchpad_release_ns) / 10000.0;
      }
      const float rx = PadAxisX(event.gtouchpad.x);
      const float ry = PadAxisY(event.gtouchpad.y);
      const float full_now = std::max(
          static_cast<float>(REXCVAR_GET(hid_sdl_touchpad_full_deflection)), 0.05f);
      const float dx = rx / full_now - controller.touchpad_lift_x;
      const float dy = ry / full_now - controller.touchpad_lift_y;
      if (gap_ms <= REXCVAR_GET(hid_sdl_touchpad_reacquire_ms) &&
          std::sqrt(dx * dx + dy * dy) <= 0.35f) {
        controller.touchpad_releasing = false;
        controller.touchpad_release_ns = 0;
        controller.touchpad_last_ns = now_ns;
        if (REXCVAR_GET(hid_sdl_touchpad_trace)) {
          REXLOG_INFO("rpad: phantom lift ignored ({:.0f}ms, moved {:.2f}) - contact continues",
                      gap_ms, std::sqrt(dx * dx + dy * dy));
        }
        // Fall through to update the target; the stick keeps its position.
        float cx = rx / full_now;
        float cy = ry / full_now;
        const float rad = std::sqrt(cx * cx + cy * cy);
        if (rad > 1.0f) {
          cx /= rad;
          cy /= rad;
        }
        controller.pad_target_x = cx;
        controller.pad_target_y = cy;
        controller.touchpad_last_move_ns = now_ns;
        return;
      }
    }
    if (!controller.touchpad_active || controller.touchpad_releasing) {
      controller.touchpad_active = true;
      controller.touchpad_releasing = false;
      controller.touchpad_finger = event.gtouchpad.finger;
      // Every new touch starts the stick at CENTRE, wherever the finger
      // landed. Skate reads the travel, not the endpoint: a stick that
      // appears at full deflection has not flicked, it has teleported, and
      // the trick does not come out. From centre the same gesture becomes a
      // real sweep - down and out for a flip, and back through centre when
      // the thumb lifts.
      controller.pad_current_x = 0.0f;
      controller.pad_current_y = 0.0f;
      controller.touchpad_release_ns = 0;
      controller.touchpad_last_ns = rex::chrono::Clock::QueryHostSystemTime();
    } else if (event.gtouchpad.finger != controller.touchpad_finger) {
      return;  // a second finger, while the first still has the stick
    }
  } else if (!controller.touchpad_active ||
             event.gtouchpad.finger != controller.touchpad_finger) {
    return;  // motion from a finger that does not own the stick
  }

  float x = PadAxisX(event.gtouchpad.x);
  float y = PadAxisY(event.gtouchpad.y);

  // The pad is square and the stick is round. A circle inside the pad is the
  // stick's full travel: everything on or outside it is full deflection in
  // whatever direction it points, so a touch in a corner snaps to the nearest
  // point ON the circle rather than reaching 1.41x what a cardinal can. That
  // per-axis clamp is exactly the artefact that makes a square pad feel wrong
  // as a round stick, and it is also what breaks rim gestures - a shuvit is a
  // slide around the edge, and it only reads as one if the edge is a circle.
  //
  // The circle is deliberately smaller than the pad. At 0.7 the outer third of
  // the pad is all full deflection, so a thumb anywhere along the bottom edge
  // is unambiguously stick-hard-down and the ollie is a big one.
  const float full = std::max(static_cast<float>(REXCVAR_GET(hid_sdl_touchpad_full_deflection)),
                              0.05f);
  x /= full;
  y /= full;
  const float radius = std::sqrt(x * x + y * y);
  if (radius > 1.0f) {
    x /= radius;
    y /= radius;
  }

  // Remember when the finger genuinely moved, so a lift can be judged against
  // it. Small jitter under a planted thumb must not count as movement.
  if (std::abs(x - controller.pad_target_x) + std::abs(y - controller.pad_target_y) > 0.02f) {
    controller.touchpad_last_move_ns = rex::chrono::Clock::QueryHostSystemTime();
  }

  controller.pad_target_x = x;
  controller.pad_target_y = y;
}

// The d-pad off the left touchpad, activated by PRESSING rather than touching.
// A thumb resting on the pad is the normal way to hold a Deck, so a
// touch-activated d-pad would fire a direction the whole time.
void SDLInputDriver::OnLeftTouchpadLocked(const SDL_Event& event, ControllerState& controller) {
  auto& pad = controller.state.gamepad;
  constexpr uint16_t kDpadMask = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                                 X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;

  // buttons is a byte-swapping wrapper, so every change is a read, a modify
  // and a write back rather than a compound assignment.
  auto clear = [&]() {
    if (controller.left_pad_buttons) {
      pad.buttons = static_cast<uint16_t>(pad.buttons & ~controller.left_pad_buttons);
      controller.left_pad_buttons = 0;
      controller.state_changed = true;
    }
  };

  if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
    if (controller.left_pad_active && event.gtouchpad.finger == controller.left_pad_finger) {
      controller.left_pad_active = false;
      clear();
    }
    return;
  }
  if (!controller.left_pad_active) {
    controller.left_pad_active = true;
    controller.left_pad_finger = event.gtouchpad.finger;
  } else if (event.gtouchpad.finger != controller.left_pad_finger) {
    return;
  }

  // A threshold of 0 means "treat any touch as a press", which is the escape
  // hatch for a pad whose pressure never crosses a threshold.
  const float threshold = static_cast<float>(REXCVAR_GET(hid_sdl_touchpad_press_threshold));
  const bool pressed = threshold <= 0.0f || event.gtouchpad.pressure >= threshold;
  if (!pressed) {
    clear();
    return;
  }

  const float x = PadAxisX(event.gtouchpad.x);
  const float y = PadAxisY(event.gtouchpad.y);
  const float deadzone = static_cast<float>(REXCVAR_GET(hid_sdl_touchpad_dpad_deadzone));
  if (std::sqrt(x * x + y * y) < deadzone) {
    clear();  // pressed in the dead middle - no direction is the honest answer
    return;
  }

  // 8-way, by which axis dominates. Both bits set on a diagonal, exactly as a
  // real d-pad reports it.
  uint16_t buttons = 0;
  if (std::abs(x) > deadzone * 0.5f) {
    buttons |= x > 0.0f ? X_INPUT_GAMEPAD_DPAD_RIGHT : X_INPUT_GAMEPAD_DPAD_LEFT;
  }
  if (std::abs(y) > deadzone * 0.5f) {
    buttons |= y > 0.0f ? X_INPUT_GAMEPAD_DPAD_UP : X_INPUT_GAMEPAD_DPAD_DOWN;
  }
  if (buttons != controller.left_pad_buttons) {
    const uint16_t without_ours =
        static_cast<uint16_t>(pad.buttons & ~(controller.left_pad_buttons & kDpadMask));
    pad.buttons = static_cast<uint16_t>(without_ours | buttons);
    controller.left_pad_buttons = buttons;
    controller.state_changed = true;
  }
}

// Steer each touchpad-driven stick toward where the finger is. Exponential
// smoothing on a real elapsed time, so the feel does not change with frame
// rate - at 30fps and at 90fps the stick takes the same wall-clock time to
// cross the pad.
void SDLInputDriver::AdvanceTouchpadSmoothingLocked() {
  const uint64_t now = rex::chrono::Clock::QueryHostSystemTime();
  const double tau_ms = REXCVAR_GET(hid_sdl_touchpad_smoothing_ms);
  for (auto& controller : controllers_) {
    if (!controller.sdl || !controller.touchpad_active) {
      continue;
    }
    // QueryHostSystemTime is in 100ns units.
    double dt_ms = 0.0;
    if (controller.touchpad_last_ns != 0 && now > controller.touchpad_last_ns) {
      dt_ms = static_cast<double>(now - controller.touchpad_last_ns) / 10000.0;
    }
    controller.touchpad_last_ns = now;

    auto& pad = controller.state.gamepad;

    // Ten times a second while a finger owns the stick. This is the half that
    // matters: a finger held still sends NO events at all, so the event trace
    // goes quiet exactly when the interesting thing happens. If the stick
    // walks back to centre while the thumb is planted, it can only show up
    // here.
    if (REXCVAR_GET(hid_sdl_touchpad_trace)) {
      static uint64_t s_last_trace_ns = 0;
      if (s_last_trace_ns == 0 || now < s_last_trace_ns ||
          (now - s_last_trace_ns) / 10000 >= 100) {
        s_last_trace_ns = now;
        REXLOG_INFO(
            "rpad tick: releasing={} target=({:+.2f},{:+.2f}) current=({:+.2f},{:+.2f}) "
            "thumb_r=({},{}) dt={:.1f}ms",
            controller.touchpad_releasing ? 1 : 0, controller.pad_target_x,
            controller.pad_target_y, controller.pad_current_x, controller.pad_current_y,
            int16_t(pad.thumb_rx), int16_t(pad.thumb_ry), dt_ms);
      }
    }

    if (controller.touchpad_releasing) {
      // The thumb is off the pad. Hold the lift position long enough that at
      // least one poll is guaranteed to see it, then let go completely - a
      // real stick springs back, it does not ease back, and easing back is
      // what the game reads as a held stick and turns into a manual.
      double held_ms = 0.0;
      if (controller.touchpad_release_ns != 0 && now > controller.touchpad_release_ns) {
        held_ms = static_cast<double>(now - controller.touchpad_release_ns) / 10000.0;
      }
      // A flick is believed at once; a lift from a planted thumb is held on to
      // for longer, because that is the one the pad lies about and the one
      // where waiting costs nothing - nothing is moving.
      double hold_ms = REXCVAR_GET(hid_sdl_touchpad_flick_hold_ms);
      if (controller.touchpad_lift_was_stationary) {
        hold_ms = std::max(hold_ms, REXCVAR_GET(hid_sdl_touchpad_reacquire_ms));
      }
      if (held_ms < hold_ms) {
        const int16_t hx = FloatToThumb(controller.pad_current_x);
        const int16_t hy = FloatToThumb(controller.pad_current_y);
        if (hx != pad.thumb_rx || hy != pad.thumb_ry) {
          pad.thumb_rx = hx;
          pad.thumb_ry = hy;
          controller.state_changed = true;
        }
        continue;
      }
      if (REXCVAR_GET(hid_sdl_touchpad_trace)) {
        REXLOG_INFO("rpad: RELEASING DONE after {:.1f}ms - stick dropped to centre", held_ms);
      }
      controller.touchpad_active = false;
      controller.touchpad_releasing = false;
      controller.pad_current_x = 0.0f;
      controller.pad_current_y = 0.0f;
      controller.pad_target_x = 0.0f;
      controller.pad_target_y = 0.0f;
      pad.thumb_rx = controller.stick_rx;
      pad.thumb_ry = controller.stick_ry;
      controller.state_changed = true;
      continue;
    }

    float alpha = 1.0f;
    if (tau_ms > 0.0 && dt_ms > 0.0) {
      alpha = static_cast<float>(1.0 - std::exp(-dt_ms / tau_ms));
    }
    controller.pad_current_x += (controller.pad_target_x - controller.pad_current_x) * alpha;
    controller.pad_current_y += (controller.pad_target_y - controller.pad_current_y) * alpha;

    const int16_t rx = FloatToThumb(controller.pad_current_x);
    const int16_t ry = FloatToThumb(controller.pad_current_y);
    if (rx != pad.thumb_rx || ry != pad.thumb_ry) {
      pad.thumb_rx = rx;
      pad.thumb_ry = ry;
      controller.state_changed = true;
    }
  }
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons:
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles:
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  assert(idx);
  auto& controller = controllers_.at(*idx);

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  // Pressed or released?
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    if (!controller) {
      continue;
    }
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(uint32_t user_index) {
  if (user_index >= controllers_.size()) {
    return nullptr;
  }
  auto controller = &controllers_.at(user_index);
  if (!controller->sdl) {
    return nullptr;
  }
  return controller;
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_INFO("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
              SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    if (!attached_window_->app_context().CallInUIThread([this]() {
          SDL_PumpEvents();
          sdl_pumpevents_queued_ = false;
        })) {
      // The call was refused, so the callback that clears the latch will never
      // run - and the latch is what lets the NEXT pump be queued. Leaving it
      // set means SDL is never pumped again for the life of the process:
      // controller state freezes at whatever it last saw, silently and
      // permanently. Clear it here so a refusal costs one missed pump instead
      // of all of them.
      sdl_pumpevents_queued_ = false;
      REXLOG_WARN("SDL: the event pump could not be queued on the UI thread");
    }
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
