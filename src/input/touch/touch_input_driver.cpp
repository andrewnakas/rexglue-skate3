#include <rex/input/touch_input_driver.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(touch_controls, true, "Input",
                    "Show on-screen touch controls and drive the guest pad from them while no "
                    "physical controller is connected")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::input::touch {
namespace {

// Sizes are fractions of the window's shorter side; centres are fractions of
// width and height. Laid out for a phone in landscape.
constexpr float kStickR = 0.155f;
constexpr float kFaceR = 0.070f;
constexpr float kShoulderR = 0.062f;
constexpr float kSmallR = 0.042f;

constexpr TouchControl kLayout[] = {
    {TouchControlId::kLeftStick, 0.135f, 0.720f, kStickR, "", true},
    {TouchControlId::kRightStick, 0.865f, 0.720f, kStickR, "", true},

    // Face buttons, arranged as on the pad, up and inboard of the right stick.
    {TouchControlId::kY, 0.888f, 0.230f, kFaceR, "Y", false},
    {TouchControlId::kB, 0.955f, 0.360f, kFaceR, "B", false},
    {TouchControlId::kA, 0.888f, 0.480f, kFaceR, "A", false},
    {TouchControlId::kX, 0.820f, 0.360f, kFaceR, "X", false},

    {TouchControlId::kLeftShoulder, 0.060f, 0.085f, kShoulderR, "LB", false},
    {TouchControlId::kLeftTrigger, 0.175f, 0.085f, kShoulderR, "LT", false},
    {TouchControlId::kRightShoulder, 0.940f, 0.085f, kShoulderR, "RB", false},
    {TouchControlId::kRightTrigger, 0.825f, 0.085f, kShoulderR, "RT", false},

    {TouchControlId::kBack, 0.435f, 0.075f, kSmallR, "\xE2\x8C\xAB", false},
    {TouchControlId::kStart, 0.565f, 0.075f, kSmallR, "\xE2\x98\xB0", false},

    // D-pad, inboard of the left stick - menus and the phone/trick book need it.
    {TouchControlId::kDPadUp, 0.300f, 0.590f, kSmallR, "\xE2\x96\xB2", false},
    {TouchControlId::kDPadDown, 0.300f, 0.850f, kSmallR, "\xE2\x96\xBC", false},
    {TouchControlId::kDPadLeft, 0.245f, 0.720f, kSmallR, "\xE2\x97\x80", false},
    {TouchControlId::kDPadRight, 0.355f, 0.720f, kSmallR, "\xE2\x96\xB6", false},
};

struct Finger {
  SDL_FingerID id = 0;
  bool down = false;
  // Which control this finger claimed when it landed. A finger keeps its
  // control until it lifts, even if it slides off - otherwise a thumb rolling
  // off the edge of a stick silently stops steering.
  int control = -1;
  float x = 0.0f, y = 0.0f;
};

constexpr size_t kMaxFingers = 10;

std::mutex g_mutex;
Finger g_fingers[kMaxFingers];
std::atomic<bool> g_physical_controller{false};

// Hit test in normalised window space. Aspect is width/height, needed because
// radii are relative to the shorter side.
int ControlAt(float x, float y, float aspect) {
  int best = -1;
  float best_d2 = 0.0f;
  for (size_t i = 0; i < std::size(kLayout); ++i) {
    const TouchControl& c = kLayout[i];
    // Convert to a space where a circle is a circle: scale x by the aspect.
    const float dx = (x - c.centre_x) * aspect;
    const float dy = y - c.centre_y;
    // Sticks get a generous pad - the thumb lands approximately.
    const float r = c.radius * (c.is_stick ? 1.15f : 1.0f);
    const float d2 = dx * dx + dy * dy;
    if (d2 <= r * r && (best < 0 || d2 < best_d2)) {
      best = int(i);
      best_d2 = d2;
    }
  }
  return best;
}

float WindowAspect() {
  // The layout only needs a ratio, and every iOS window here is the display.
  int w = 0, h = 0;
  if (const SDL_DisplayID display = SDL_GetPrimaryDisplay()) {
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display)) {
      w = mode->w;
      h = mode->h;
    }
  }
  return (w > 0 && h > 0) ? float(w) / float(h) : 16.0f / 9.0f;
}

bool HandleFingerEvent(const SDL_Event& e) {
  const float aspect = WindowAspect();
  std::lock_guard<std::mutex> lock(g_mutex);
  switch (e.type) {
    case SDL_EVENT_FINGER_DOWN: {
      for (Finger& f : g_fingers) {
        if (f.down) continue;
        f.id = e.tfinger.fingerID;
        f.down = true;
        f.x = e.tfinger.x;
        f.y = e.tfinger.y;
        f.control = ControlAt(f.x, f.y, aspect);
        return true;
      }
      return true;  // more than ten fingers; nothing sensible to do
    }
    case SDL_EVENT_FINGER_MOTION: {
      for (Finger& f : g_fingers) {
        if (f.down && f.id == e.tfinger.fingerID) {
          f.x = e.tfinger.x;
          f.y = e.tfinger.y;
          return true;
        }
      }
      return true;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      for (Finger& f : g_fingers) {
        if (f.down && f.id == e.tfinger.fingerID) {
          f = {};
          return true;
        }
      }
      return true;
    }
    default:
      return true;
  }
}

bool SDLCALL TouchEventWatch(void* /*userdata*/, SDL_Event* event) {
  if (event == nullptr) {
    return true;
  }
  switch (event->type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
      return HandleFingerEvent(*event);
    default:
      return true;
  }
}

// Reads the fingers into a pad state. Shared by GetState and the overlay so
// the two can never disagree about what is pressed.
TouchVisualState Sample() {
  TouchVisualState out;
  out.active = TouchControlsActive();
  if (!out.active) {
    return out;
  }
  const float aspect = WindowAspect();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Finger& f : g_fingers) {
    if (!f.down || f.control < 0) {
      continue;
    }
    const TouchControl& c = kLayout[f.control];
    out.pressed[size_t(c.id)] = true;
    if (!c.is_stick) {
      continue;
    }
    // Offset from the stick's centre, normalised to its radius and clamped to
    // the unit circle. y is flipped: screen y grows downward, a thumbstick's
    // does not.
    float dx = (f.x - c.centre_x) * aspect / c.radius;
    float dy = -(f.y - c.centre_y) / c.radius;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1.0f) {
      dx /= len;
      dy /= len;
    }
    if (c.id == TouchControlId::kLeftStick) {
      out.left_x = dx;
      out.left_y = dy;
    } else {
      out.right_x = dx;
      out.right_y = dy;
    }
  }
  return out;
}

int16_t ToAxis(float v) {
  const float scaled = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
  return int16_t(std::lround(scaled));
}

}  // namespace

const TouchControl* TouchLayout(size_t* count_out) {
  if (count_out != nullptr) {
    *count_out = std::size(kLayout);
  }
  return kLayout;
}

TouchVisualState GetTouchVisualState() { return Sample(); }

bool TouchControlsActive() {
  return REXCVAR_GET(touch_controls) && !g_physical_controller.load(std::memory_order_relaxed);
}

void SetPhysicalControllerConnected(bool connected) {
  const bool was = g_physical_controller.exchange(connected, std::memory_order_relaxed);
  if (was != connected) {
    REXLOG_INFO("touch: physical controller {}; on-screen controls {}",
                connected ? "connected" : "disconnected", connected ? "hidden" : "shown");
    // Drop any fingers still held, so a control cannot latch on across the
    // switch and leave the guest holding a button nobody is touching.
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Finger& f : g_fingers) {
      f = {};
    }
  }
}

TouchInputDriver::TouchInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

TouchInputDriver::~TouchInputDriver() {
  if (watch_installed_) {
    SDL_RemoveEventWatch(TouchEventWatch, nullptr);
  }
}

X_STATUS TouchInputDriver::Setup() {
  if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
    REXLOG_ERROR("touch: SDL events subsystem unavailable: {}", SDL_GetError());
    return X_STATUS_UNSUCCESSFUL;
  }
  // A watch rather than the event queue: touch has to be sampled even while
  // the guest is not pumping events, and consuming them here would take them
  // away from the host UI.
  if (!SDL_AddEventWatch(TouchEventWatch, nullptr)) {
    REXLOG_ERROR("touch: could not install the touch event watch: {}", SDL_GetError());
    return X_STATUS_UNSUCCESSFUL;
  }
  watch_installed_ = true;
  REXLOG_INFO("touch: on-screen controls ready");
  return X_STATUS_SUCCESS;
}

X_RESULT TouchInputDriver::GetCapabilities(uint32_t user_index, uint32_t /*flags*/,
                                           X_INPUT_CAPABILITIES* out_caps) {
  if (user_index != 0 || !TouchControlsActive()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // gamepad
  out_caps->sub_type = 0x01;  // standard
  out_caps->flags = 0;
  // Report the full set: the guest decides what to offer based on this, and a
  // control the overlay does not draw simply never reports pressed.
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(0xFFC0);
  out_caps->gamepad.thumb_ly = int16_t(0xFFC0);
  out_caps->gamepad.thumb_rx = int16_t(0xFFC0);
  out_caps->gamepad.thumb_ry = int16_t(0xFFC0);
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index != 0 || !TouchControlsActive()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const TouchVisualState s = Sample();
  std::memset(out_state, 0, sizeof(*out_state));

  uint16_t buttons = 0;
  auto set = [&](TouchControlId id, uint16_t mask) {
    if (s.pressed[size_t(id)]) {
      buttons |= mask;
    }
  };
  set(TouchControlId::kDPadUp, 0x0001);
  set(TouchControlId::kDPadDown, 0x0002);
  set(TouchControlId::kDPadLeft, 0x0004);
  set(TouchControlId::kDPadRight, 0x0008);
  set(TouchControlId::kStart, 0x0010);
  set(TouchControlId::kBack, 0x0020);
  set(TouchControlId::kLeftShoulder, 0x0100);
  set(TouchControlId::kRightShoulder, 0x0200);
  set(TouchControlId::kA, 0x1000);
  set(TouchControlId::kB, 0x2000);
  set(TouchControlId::kX, 0x4000);
  set(TouchControlId::kY, 0x8000);
  out_state->gamepad.buttons = buttons;

  // Triggers are on/off from a touch; full travel is what the game expects
  // for a grab or a brake.
  out_state->gamepad.left_trigger = s.pressed[size_t(TouchControlId::kLeftTrigger)] ? 255 : 0;
  out_state->gamepad.right_trigger = s.pressed[size_t(TouchControlId::kRightTrigger)] ? 255 : 0;

  out_state->gamepad.thumb_lx = ToAxis(s.left_x);
  out_state->gamepad.thumb_ly = ToAxis(s.left_y);
  out_state->gamepad.thumb_rx = ToAxis(s.right_x);
  out_state->gamepad.thumb_ry = ToAxis(s.right_y);

  // The guest polls this to notice change; a monotonic counter is enough and
  // avoids having to diff the whole struct.
  static std::atomic<uint32_t> s_packet{0};
  out_state->packet_number = s_packet.fetch_add(1, std::memory_order_relaxed);
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetStateUi(uint32_t /*user_index*/, X_INPUT_STATE* /*out_state*/) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT TouchInputDriver::SetState(uint32_t /*user_index*/, X_INPUT_VIBRATION* /*vibration*/) {
  // No haptics: iOS taptics are not a rumble motor and firing them per frame
  // would be worse than silence.
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetKeystroke(uint32_t /*user_index*/, uint32_t /*flags*/,
                                        X_INPUT_KEYSTROKE* /*out_keystroke*/) {
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::touch
