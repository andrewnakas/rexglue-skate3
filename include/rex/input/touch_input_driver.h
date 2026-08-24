// On-screen touch controls: a virtual Xbox pad driven by the touchscreen,
// used when no physical controller is attached.
//
// The layout lives here rather than in the driver's translation unit because
// the overlay that draws the controls has to agree with the driver about
// where they are, exactly - a button the player can see but not press, or the
// reverse, is worse than no touch support at all. Both read these.
//
// Coordinates are fractions of the window, origin top-left, so the layout is
// resolution independent.

#ifndef REX_INPUT_TOUCH_INPUT_DRIVER_H_
#define REX_INPUT_TOUCH_INPUT_DRIVER_H_

#include <array>
#include <atomic>
#include <cstdint>

#include <rex/input/input_driver.h>

namespace rex::input::touch {

// Every control the overlay can draw and the driver can press. Order is the
// draw order; nothing else depends on it.
enum class TouchControlId : uint32_t {
  kLeftStick,
  kRightStick,
  kA,
  kB,
  kX,
  kY,
  kLeftShoulder,
  kRightShoulder,
  kLeftTrigger,
  kRightTrigger,
  kStart,
  kBack,
  kDPadUp,
  kDPadDown,
  kDPadLeft,
  kDPadRight,
  kCount,
};

struct TouchControl {
  TouchControlId id;
  // Centre and radius as a fraction of the window's SHORTER side for radius,
  // and of width/height for the centre. Using the shorter side for size keeps
  // controls the same physical size whatever the aspect ratio.
  float centre_x;
  float centre_y;
  float radius;
  const char* label;
  // Sticks report an axis; everything else is a button.
  bool is_stick;
};

// The layout, in draw order. Sized for a phone held in landscape: sticks under
// the thumbs, face buttons within reach above the right stick, shoulders and
// triggers along the top edge where the index fingers rest.
const TouchControl* TouchLayout(size_t* count_out);

// What the overlay needs to render: which controls are held, and how far each
// stick has been pushed (-1..1, y up).
struct TouchVisualState {
  std::array<bool, size_t(TouchControlId::kCount)> pressed{};
  float left_x = 0.0f, left_y = 0.0f;
  float right_x = 0.0f, right_y = 0.0f;
  // False while a real controller is attached - the overlay hides itself.
  bool active = false;
};

TouchVisualState GetTouchVisualState();

// True when the touch pad is supplying input, i.e. it is enabled and no
// physical controller is connected.
bool TouchControlsActive();

// Tells the touch driver whether a physical controller is present. Called by
// the gamepad driver as controllers come and go.
void SetPhysicalControllerConnected(bool connected);

class TouchInputDriver final : public InputDriver {
 public:
  TouchInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~TouchInputDriver() override;

  X_STATUS Setup() override;
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  // The host UI takes touch directly; emulating a pad for it would fight the
  // pointer, so report nothing here (same reasoning as the keyboard driver).
  X_RESULT GetStateUi(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  bool watch_installed_ = false;
};

}  // namespace rex::input::touch

#endif  // REX_INPUT_TOUCH_INPUT_DRIVER_H_
