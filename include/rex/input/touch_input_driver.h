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
#include <functional>

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
  // Opens the recomp settings. NOT a guest button: it presses nothing and the
  // game never sees it.
  //
  // The settings were reachable only by the RB + Start chord, which on a
  // touchscreen means hitting two on-screen buttons at opposite corners at the
  // same instant. It works, and nobody discovers it. A tester's first question
  // about the settings menu was where several of its features had gone; the
  // answer for a phone player was that the menu itself was most of the way to
  // hidden.
  kMenu,
  kCount,
};

// What a control is drawn AS. The four direction glyphs, the Start bars and
// the Back chevron used to be Unicode in the label - and the ImGui default
// font has none of those code points, so six of the sixteen controls came out
// as literal question marks on every phone. Visible in the first screenshot
// anyone sent. Shapes are drawn, so there is no font to be missing.
enum class TouchGlyph : uint32_t {
  kNone,      // sticks: the well and thumb pad
  kLabel,     // draw `label` as text (A/B/X/Y/LB/RB/LT/RT - all plain ASCII)
  kUp,
  kDown,
  kLeft,
  kRight,
  kStart,     // two horizontal bars
  kBack,      // a left-pointing chevron
  kMenu,      // a gear
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
  TouchGlyph glyph = TouchGlyph::kLabel;
};

// The layout, in draw order. Sized for a phone held in landscape: sticks under
// the thumbs, face buttons within reach above the right stick, shoulders and
// triggers along the top edge where the index fingers rest.
//
// The shipped arrangement is a starting point, not a verdict: it was drawn for
// one phone and the first thing testers asked for was to move things. Whatever
// the player has saved is overlaid here, so the hit test and the renderer
// still read one table and still cannot disagree about where a control is.
const TouchControl* TouchLayout(size_t* count_out);

// ---- Player layout -------------------------------------------------------
//
// Positions and sizes only. Which controls exist, what they do and the order
// they draw in stay in code, because none of those are a matter of taste and
// a file that could change them is a file that can break the pad.

// Where the layout is stored. Set once at startup from the user data root;
// until then nothing is loaded and nothing is saved.
void SetTouchLayoutPath(const char* path);

// Replace one control's placement. Fractions, in the same units as the table.
// Out-of-range values are clamped so a control cannot be put off-screen.
void SetTouchControlPlacement(TouchControlId id, float centre_x, float centre_y,
                              float radius);

// Back to the shipped arrangement, in memory and on disk.
void ResetTouchLayout();

// Write the current layout out. Called when the editor is dismissed.
bool SaveTouchLayout();

// ---- Layout editing ------------------------------------------------------
//
// While this is on, the driver presses nothing: a finger moves the control it
// lands on instead. The guest sees no input at all, which is the point - you
// cannot rearrange a pad while the pad is playing the game.
void SetTouchLayoutEditing(bool editing);
bool TouchLayoutEditing();

// Which control the editor is holding, or kCount for none. The overlay reads
// it to highlight what is being moved.
TouchControlId TouchLayoutHeldControl();

// What the overlay needs to render: which controls are held, and how far each
// stick has been pushed (-1..1, y up).
struct TouchVisualState {
  std::array<bool, size_t(TouchControlId::kCount)> pressed{};
  float left_x = 0.0f, left_y = 0.0f;
  float right_x = 0.0f, right_y = 0.0f;
  // False while a real controller is attached - the overlay hides itself.
  bool active = false;
  // The layout editor is open: the overlay draws the controls as movable and
  // the pad reports nothing.
  bool editing = false;
  // While editing, the control a finger is currently dragging.
  TouchControlId held = TouchControlId::kCount;
};

TouchVisualState GetTouchVisualState();

// True when the touch pad is supplying input, i.e. it is enabled and no
// physical controller is connected.
bool TouchControlsActive();

// Tells the touch driver whether a physical controller is present. Called by
// the gamepad driver as controllers come and go.
void SetPhysicalControllerConnected(bool connected);

// What the on-screen menu button does. Fired on release, from the SDL event
// thread, so the handler must hop to the UI thread itself - the app's own
// chord callbacks already do.
void SetMenuButtonCallback(std::function<void()> callback);

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
