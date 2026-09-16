#include <rex/input/touch_input_driver.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(touch_stick_size, 1.25, "Input",
                      "Size of the on-screen thumbsticks, as a multiple of the original. "
                      "Bigger sticks give the thumb more room and are easier to land on "
                      "without looking.")
    .range(0.75, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(touch_stick_saturation, 0.72, "Input",
                      "How far the thumb has to travel for FULL stick deflection, as a "
                      "fraction of the stick's radius. 1.0 means the very edge, which is "
                      "what made a hard turn hard to reach - the thumb runs out of comfort "
                      "before the stick runs out of travel. Lower saturates sooner.")
    .range(0.4, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(touch_opacity, 1.0, "Input",
                      "How visible the on-screen controls are, as a multiple of their "
                      "normal alpha. They are deliberately faint - they sit over the game "
                      "for the whole session - but faint enough to lose against bright "
                      "concrete on some screens. 0 draws nothing while the controls still "
                      "work, for anyone who has learnt where they are.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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
    {TouchControlId::kLeftStick, 0.135f, 0.720f, kStickR, "Left stick", true,
     TouchGlyph::kNone},
    {TouchControlId::kRightStick, 0.865f, 0.720f, kStickR, "Right stick", true,
     TouchGlyph::kNone},

    // Face buttons, arranged as on the pad, up and inboard of the right stick.
    {TouchControlId::kY, 0.888f, 0.230f, kFaceR, "Y", false},
    {TouchControlId::kB, 0.955f, 0.360f, kFaceR, "B", false},
    {TouchControlId::kA, 0.888f, 0.480f, kFaceR, "A", false},
    {TouchControlId::kX, 0.820f, 0.360f, kFaceR, "X", false},

    {TouchControlId::kLeftShoulder, 0.060f, 0.085f, kShoulderR, "LB", false},
    {TouchControlId::kLeftTrigger, 0.175f, 0.085f, kShoulderR, "LT", false},
    {TouchControlId::kRightShoulder, 0.940f, 0.085f, kShoulderR, "RB", false},
    {TouchControlId::kRightTrigger, 0.825f, 0.085f, kShoulderR, "RT", false},

    // These six were Unicode glyphs - back-arrow, hamburger, four triangles.
    // The ImGui default font carries none of them, so all six rendered as "?"
    // on every device, which is what the first bug screenshot shows. Drawn as
    // shapes now; the label is kept only as a name for the layout editor.
    {TouchControlId::kBack, 0.435f, 0.075f, kSmallR, "Back", false,
     TouchGlyph::kBack},
    {TouchControlId::kStart, 0.565f, 0.075f, kSmallR, "Start", false,
     TouchGlyph::kStart},

    // D-pad, inboard of the left stick - menus and the phone/trick book need it.
    {TouchControlId::kDPadUp, 0.300f, 0.590f, kSmallR, "Up", false,
     TouchGlyph::kUp},
    {TouchControlId::kDPadDown, 0.300f, 0.850f, kSmallR, "Down", false,
     TouchGlyph::kDown},
    {TouchControlId::kDPadLeft, 0.245f, 0.720f, kSmallR, "Left", false,
     TouchGlyph::kLeft},
    {TouchControlId::kDPadRight, 0.355f, 0.720f, kSmallR, "Right", false,
     TouchGlyph::kRight},

    // Top centre, between Back and Start and clear of both. Presses nothing in
    // the game - see GetState, which never reads it.
    {TouchControlId::kMenu, 0.500f, 0.075f, kSmallR, "Menu", false,
     TouchGlyph::kMenu},
};

struct Finger {
  SDL_FingerID id = 0;
  bool down = false;
  // Which control this finger claimed when it landed. A finger keeps its
  // control until it lifts, even if it slides off - otherwise a thumb rolling
  // off the edge of a stick silently stops steering.
  int control = -1;
  float x = 0.0f, y = 0.0f;
  // Where it landed, kept so a tap can be told from a drag. While the layout
  // is being edited the control travels with the finger, so "still on the
  // control" is true however far it was dragged and cannot answer that on its
  // own.
  float down_x = 0.0f, down_y = 0.0f;
};

constexpr size_t kMaxFingers = 10;

std::mutex g_mutex;
Finger g_fingers[kMaxFingers];
std::atomic<bool> g_physical_controller{false};

// Fired on release of the on-screen menu button. Guarded by g_menu_mutex
// rather than being an atomic, because a std::function is not one.
std::mutex g_menu_mutex;
std::function<void()> g_menu_callback;

// ---- The player's own arrangement ----------------------------------------
//
// One override per control, each field optional. Absent means "whatever the
// shipped table says", so a saved file from an older build keeps working when
// a control is added, and a control the player never touched still moves if
// the default is ever revised.
struct Placement {
  bool has_centre = false;
  bool has_radius = false;
  float centre_x = 0.0f;
  float centre_y = 0.0f;
  float radius = 0.0f;
};

std::mutex g_layout_mutex;
std::array<Placement, size_t(TouchControlId::kCount)> g_placements;
std::string g_layout_path;
std::atomic<bool> g_layout_dirty{true};
std::atomic<bool> g_layout_editing{false};
std::atomic<uint32_t> g_held_control{uint32_t(TouchControlId::kCount)};
// Survives the finger lifting, unlike g_held_control: the editor's panel acts
// on it, and a selection that vanished on release would make the size buttons
// unusable.
std::atomic<uint32_t> g_selected_control{uint32_t(TouchControlId::kCount)};
// The editor panel's own area, left to the UI rather than treated as canvas.
std::atomic<float> g_reserved_x0{0.0f}, g_reserved_y0{0.0f};
std::atomic<float> g_reserved_x1{0.0f}, g_reserved_y1{0.0f};

bool InReservedRect(float x, float y) {
  const float x0 = g_reserved_x0.load(std::memory_order_relaxed);
  const float x1 = g_reserved_x1.load(std::memory_order_relaxed);
  if (x1 <= x0) {
    return false;
  }
  return x >= x0 && x <= x1 && y >= g_reserved_y0.load(std::memory_order_relaxed) &&
         y <= g_reserved_y1.load(std::memory_order_relaxed);
}

// A control's own name in the file. Index would be shorter and would also make
// the file meaningless to read and impossible to reorder, and this is a file
// someone may well open.
const char* ControlKey(TouchControlId id) {
  switch (id) {
    case TouchControlId::kLeftStick: return "left_stick";
    case TouchControlId::kRightStick: return "right_stick";
    case TouchControlId::kA: return "a";
    case TouchControlId::kB: return "b";
    case TouchControlId::kX: return "x";
    case TouchControlId::kY: return "y";
    case TouchControlId::kLeftShoulder: return "lb";
    case TouchControlId::kRightShoulder: return "rb";
    case TouchControlId::kLeftTrigger: return "lt";
    case TouchControlId::kRightTrigger: return "rt";
    case TouchControlId::kStart: return "start";
    case TouchControlId::kBack: return "back";
    case TouchControlId::kDPadUp: return "dpad_up";
    case TouchControlId::kDPadDown: return "dpad_down";
    case TouchControlId::kDPadLeft: return "dpad_left";
    case TouchControlId::kDPadRight: return "dpad_right";
    case TouchControlId::kMenu: return "menu";
    default: return "";
  }
}

// Kept inside the screen, and above a size a thumb can land on. A layout that
// puts a button where it cannot be pressed is worse than the default one, and
// the editor is driven by a thumb that can easily slide off the edge.
// How far a finger may travel and still count as a tap rather than a drag.
// Two percent of the screen height: comfortably inside the slop of a thumb
// held still, and far below any deliberate move.
constexpr float kTapRadius = 0.02f;
constexpr float kMinRadius = 0.025f;
constexpr float kMaxRadius = 0.260f;

void ClampPlacement(float& cx, float& cy, float& r) {
  r = std::clamp(r, kMinRadius, kMaxRadius);
  cx = std::clamp(cx, 0.02f, 0.98f);
  cy = std::clamp(cy, 0.04f, 0.96f);
}

void LoadTouchLayoutLocked() {
  g_placements.fill(Placement{});
  if (g_layout_path.empty()) {
    return;
  }
  SDL_IOStream* io = SDL_IOFromFile(g_layout_path.c_str(), "rb");
  if (io == nullptr) {
    return;  // no file yet is the normal case, not an error
  }
  size_t size = 0;
  void* data = SDL_LoadFile_IO(io, &size, true);
  if (data == nullptr) {
    return;
  }
  // "key = cx cy r" per line, '#' comments. Deliberately not TOML: this is
  // read by the input driver, which has no parser and no business gaining one.
  std::string text(static_cast<const char*>(data), size);
  SDL_free(data);
  size_t pos = 0;
  int loaded = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = (eol == std::string::npos) ? text.size() : eol + 1;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    char key[32] = {};
    float cx = 0.0f, cy = 0.0f, r = 0.0f;
    if (SDL_sscanf(line.c_str(), "%31s = %f %f %f", key, &cx, &cy, &r) != 4) {
      continue;
    }
    for (size_t i = 0; i < size_t(TouchControlId::kCount); ++i) {
      if (SDL_strcmp(key, ControlKey(TouchControlId(i))) != 0) {
        continue;
      }
      ClampPlacement(cx, cy, r);
      g_placements[i] = {true, true, cx, cy, r};
      ++loaded;
      break;
    }
  }
  if (loaded > 0) {
    REXLOG_INFO("touch: loaded {} control placements from {}", loaded, g_layout_path);
  }
}

// Hit test in normalised window space. Aspect is width/height, needed because
// radii are relative to the shorter side.
//
// Reads the EFFECTIVE layout, not the shipped constant. It used to read
// kLayout directly, which was fine while the layout could not change and is
// the exact way a movable control would end up drawn in one place and pressed
// in another - the failure the header warns about.
int ControlAt(float x, float y, float aspect) {
  size_t count = 0;
  const TouchControl* layout = TouchLayout(&count);
  int best = -1;
  float best_d2 = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const TouchControl& c = layout[i];
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

bool IsMenuControl(int index) {
  size_t count = 0;
  const TouchControl* layout = TouchLayout(&count);
  return index >= 0 && size_t(index) < count && layout[index].id == TouchControlId::kMenu;
}

void FireMenuCallback() {
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(g_menu_mutex);
    callback = g_menu_callback;
  }
  if (callback) {
    callback();
  }
}

// While the editor is open a finger MOVES the control it landed on instead of
// pressing it. Two fingers on the same control resize it by the change in the
// distance between them, which is the gesture everyone tries first.
bool HandleFingerEventEditing(const SDL_Event& e, float aspect) {
  std::unique_lock<std::mutex> lock(g_mutex);
  switch (e.type) {
    case SDL_EVENT_FINGER_DOWN: {
      // The panel's own area belongs to the panel. Without this, a control
      // dragged under the panel would swallow every press meant for its
      // buttons - including Done, which is the way out.
      if (InReservedRect(e.tfinger.x, e.tfinger.y)) {
        return true;
      }
      const int control = ControlAt(e.tfinger.x, e.tfinger.y, aspect);
      for (Finger& f : g_fingers) {
        if (f.down) continue;
        f = {};
        f.id = e.tfinger.fingerID;
        f.down = true;
        f.x = e.tfinger.x;
        f.y = e.tfinger.y;
        f.down_x = e.tfinger.x;
        f.down_y = e.tfinger.y;
        f.control = control;
        break;
      }
      const uint32_t id = control >= 0 ? uint32_t(TouchLayout(nullptr)[control].id)
                                       : uint32_t(TouchControlId::kCount);
      g_held_control.store(id);
      // Touching a control selects it; touching empty space keeps the last
      // selection, so the size buttons do not disarm themselves every time a
      // thumb brushes the background.
      if (control >= 0) {
        g_selected_control.store(id);
      }
      return true;
    }
    case SDL_EVENT_FINGER_MOTION: {
      Finger* moved = nullptr;
      for (Finger& f : g_fingers) {
        if (f.down && f.id == e.tfinger.fingerID) {
          moved = &f;
          break;
        }
      }
      if (moved == nullptr || moved->control < 0) {
        return true;
      }
      const float prev_x = moved->x;
      const float prev_y = moved->y;
      moved->x = e.tfinger.x;
      moved->y = e.tfinger.y;

      size_t count = 0;
      const TouchControl* layout = TouchLayout(&count);
      if (size_t(moved->control) >= count) {
        return true;
      }
      const TouchControl& c = layout[moved->control];

      // A second finger on the same control means resize, not move.
      const Finger* other = nullptr;
      for (const Finger& f : g_fingers) {
        if (&f != moved && f.down && f.control == moved->control) {
          other = &f;
          break;
        }
      }
      if (other != nullptr) {
        const auto span = [aspect](float ax, float ay, float bx, float by) {
          const float dx = (ax - bx) * aspect;
          const float dy = ay - by;
          return std::sqrt(dx * dx + dy * dy);
        };
        const float before = span(prev_x, prev_y, other->x, other->y);
        const float after = span(moved->x, moved->y, other->x, other->y);
        SetTouchControlPlacement(c.id, c.centre_x, c.centre_y,
                                 c.radius + (after - before) * 0.5f);
        return true;
      }
      SetTouchControlPlacement(c.id, c.centre_x + (moved->x - prev_x),
                               c.centre_y + (moved->y - prev_y), c.radius);
      return true;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      // Tapping the gear leaves the editor, exactly as it enters the settings
      // everywhere else - and it is the only exit that does not go through the
      // editor's own panel.
      //
      // That matters more than it looks. This mode takes the pad away
      // completely (Sample returns nothing while editing), so Start, Back and
      // every chord are dead, and until now the gear was dead too - it was
      // just another thing to drag. The panel's Done button was the single way
      // back to the game on a phone with no controller, and a player whose
      // panel does not appear has nothing left but to kill the app. One
      // reported exactly that.
      //
      // A tap, not a drag: the control follows the finger here, so "released
      // while still on it" is true however far it travelled. The distance from
      // where the finger landed is the thing that separates the two, measured
      // in the same aspect-corrected space as the hit test.
      bool fire_menu = false;
      for (Finger& f : g_fingers) {
        if (f.down && f.id == e.tfinger.fingerID) {
          if (e.type == SDL_EVENT_FINGER_UP && f.control >= 0 && IsMenuControl(f.control)) {
            const float dx = (e.tfinger.x - f.down_x) * aspect;
            const float dy = e.tfinger.y - f.down_y;
            fire_menu = dx * dx + dy * dy <= kTapRadius * kTapRadius;
          }
          f = {};
          break;
        }
      }
      g_held_control.store(uint32_t(TouchControlId::kCount));
      if (fire_menu) {
        // Not under the lock: the callback opens the settings, which turns
        // editing off, which takes this same mutex.
        lock.unlock();
        FireMenuCallback();
      }
      return true;
    }
    default:
      return true;
  }
}

bool HandleFingerEvent(const SDL_Event& e) {
  const float aspect = WindowAspect();
  if (TouchLayoutEditing()) {
    return HandleFingerEventEditing(e, aspect);
  }
  // Decided under the lock, acted on after it. The menu callback goes off to
  // the UI thread and has no business being invoked while this thread holds
  // the finger state every other reader needs.
  bool fire_menu = false;
  {
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
          break;
        }
        break;  // more than ten fingers; nothing sensible to do
      }
      case SDL_EVENT_FINGER_MOTION: {
        for (Finger& f : g_fingers) {
          if (f.down && f.id == e.tfinger.fingerID) {
            f.x = e.tfinger.x;
            f.y = e.tfinger.y;
            break;
          }
        }
        break;
      }
      case SDL_EVENT_FINGER_UP:
      case SDL_EVENT_FINGER_CANCELED: {
        for (Finger& f : g_fingers) {
          if (f.down && f.id == e.tfinger.fingerID) {
            // The menu button acts on RELEASE, and only if the finger is still
            // on it. A button that opens a menu the instant it is touched cannot
            // be backed out of by sliding off, which is the one escape route a
            // touchscreen has for a mis-press.
            fire_menu = e.type == SDL_EVENT_FINGER_UP && f.control >= 0 &&
                        IsMenuControl(f.control) &&
                        ControlAt(f.x, f.y, aspect) == f.control;
            f = {};
            break;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  if (fire_menu) {
    FireMenuCallback();
  }
  return true;
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
  // While the layout is being edited the pad reports NOTHING. A finger is
  // moving a button, not pressing it, and letting both happen would have the
  // skater bail every time someone repositioned the jump button.
  out.editing = TouchLayoutEditing();
  if (out.editing) {
    out.held = TouchLayoutHeldControl();
    return out;
  }
  const float aspect = WindowAspect();
  size_t layout_count = 0;
  const TouchControl* layout = TouchLayout(&layout_count);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Finger& f : g_fingers) {
    if (!f.down || f.control < 0 || size_t(f.control) >= layout_count) {
      continue;
    }
    // The EFFECTIVE layout. Reading kLayout here would have the deflection
    // measured from wherever the stick shipped rather than from where the
    // player put it.
    const TouchControl& c = layout[f.control];
    out.pressed[size_t(c.id)] = true;
    if (!c.is_stick) {
      continue;
    }
    // Offset from the stick's centre, normalised to its radius and clamped to
    // the unit circle. y is flipped: screen y grows downward, a thumbstick's
    // does not.
    // Normalise against a FRACTION of the radius, so full deflection arrives
    // before the rim. Dividing by the radius itself meant a hard turn needed
    // the thumb at the very edge of the well - reachable on paper, awkward in
    // practice, and the thing players reported as not being able to turn fully.
    const float travel = c.radius * std::clamp(float(REXCVAR_GET(touch_stick_saturation)),
                                               0.4f, 1.0f);
    float dx = (f.x - c.centre_x) * aspect / travel;
    float dy = -(f.y - c.centre_y) / travel;
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
  // Resolved here rather than at each use site so that the renderer, the hit
  // test and the deflection maths cannot disagree about where a control is or
  // how big it is - they all read this. That was already the rule for the
  // stick size; the player's own placements join it rather than being applied
  // somewhere else.
  static std::array<TouchControl, std::size(kLayout)> resolved;
  static float applied_scale = -1.0f;
  const float scale = float(REXCVAR_GET(touch_stick_size));
  const bool dirty = g_layout_dirty.exchange(false) || scale != applied_scale;
  if (dirty) {
    applied_scale = scale;
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    for (size_t i = 0; i < std::size(kLayout); ++i) {
      resolved[i] = kLayout[i];
      const Placement& p = g_placements[size_t(kLayout[i].id)];
      if (p.has_centre) {
        resolved[i].centre_x = p.centre_x;
        resolved[i].centre_y = p.centre_y;
      }
      if (p.has_radius) {
        resolved[i].radius = p.radius;
      } else if (resolved[i].is_stick) {
        // The stick-size setting still applies to a stick the player has not
        // placed by hand. One that they HAVE placed carries its own radius and
        // the setting would fight it.
        //
        // Capped so a large stick cannot walk off the bottom of the screen:
        // the centre sits at 0.72 of the height and the radius is a fraction
        // of the shorter side, so anything past ~0.27 would clip.
        resolved[i].radius = std::min(kLayout[i].radius * scale, kMaxRadius);
      }
    }
  }
  return resolved.data();
}

void SetTouchLayoutPath(const char* path) {
  std::lock_guard<std::mutex> lock(g_layout_mutex);
  g_layout_path = (path != nullptr) ? path : "";
  LoadTouchLayoutLocked();
  g_layout_dirty.store(true);
}

void SetTouchControlPlacement(TouchControlId id, float centre_x, float centre_y,
                              float radius) {
  if (size_t(id) >= size_t(TouchControlId::kCount)) {
    return;
  }
  ClampPlacement(centre_x, centre_y, radius);
  {
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    g_placements[size_t(id)] = {true, true, centre_x, centre_y, radius};
  }
  g_layout_dirty.store(true);
}

void ResetTouchLayout() {
  {
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    g_placements.fill(Placement{});
    if (!g_layout_path.empty()) {
      SDL_RemovePath(g_layout_path.c_str());
    }
  }
  g_layout_dirty.store(true);
  REXLOG_INFO("touch: layout reset to the shipped arrangement");
}

bool SaveTouchLayout() {
  std::lock_guard<std::mutex> lock(g_layout_mutex);
  if (g_layout_path.empty()) {
    return false;
  }
  std::string text =
      "# Skate 3 on-screen control layout.\n"
      "# One line per control: <name> = <centre x> <centre y> <radius>\n"
      "# Centres are fractions of the window; the radius is a fraction of the\n"
      "# shorter side. Delete a line to put that control back where it shipped,\n"
      "# or delete the file for all of them.\n";
  int written = 0;
  for (size_t i = 0; i < size_t(TouchControlId::kCount); ++i) {
    const Placement& p = g_placements[i];
    if (!p.has_centre && !p.has_radius) {
      continue;  // untouched: say nothing and keep following the default
    }
    char line[96];
    SDL_snprintf(line, sizeof(line), "%s = %.4f %.4f %.4f\n",
                 ControlKey(TouchControlId(i)), p.centre_x, p.centre_y, p.radius);
    text += line;
    ++written;
  }
  SDL_IOStream* io = SDL_IOFromFile(g_layout_path.c_str(), "wb");
  if (io == nullptr) {
    REXLOG_WARN("touch: could not write the layout to {}: {}", g_layout_path, SDL_GetError());
    return false;
  }
  const bool ok = SDL_WriteIO(io, text.data(), text.size()) == text.size();
  SDL_CloseIO(io);
  if (ok) {
    REXLOG_INFO("touch: saved {} control placements to {}", written, g_layout_path);
  }
  return ok;
}

void SetTouchLayoutEditing(bool editing) {
  const bool was = g_layout_editing.exchange(editing);
  if (was == editing) {
    return;
  }
  // Drop every finger across the switch, both ways. Going in, a finger already
  // down would otherwise start dragging whatever it happened to be resting on;
  // coming out, one still held would leave the guest pressing a button nobody
  // is touching - the same reason the controller-connected switch clears them.
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Finger& f : g_fingers) {
      f = {};
    }
  }
  g_held_control.store(uint32_t(TouchControlId::kCount));
  if (!editing) {
    g_selected_control.store(uint32_t(TouchControlId::kCount));
    SetTouchLayoutReservedRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // WARN, not INFO: Android ships at warn, and every line describing what the
  // touch controls were doing was invisible in the reports that were about the
  // touch controls.
  REXLOG_WARN("touch: layout editing {}", editing ? "on" : "off");
  if (!editing) {
    SaveTouchLayout();
  }
}

bool TouchLayoutEditing() { return g_layout_editing.load(std::memory_order_relaxed); }

TouchControlId TouchLayoutHeldControl() {
  return TouchControlId(g_held_control.load(std::memory_order_relaxed));
}

TouchControlId TouchLayoutSelectedControl() {
  return TouchControlId(g_selected_control.load(std::memory_order_relaxed));
}

void NudgeTouchControlSize(float factor) {
  const TouchControlId id = TouchLayoutSelectedControl();
  if (size_t(id) >= size_t(TouchControlId::kCount)) {
    return;
  }
  size_t count = 0;
  const TouchControl* layout = TouchLayout(&count);
  for (size_t i = 0; i < count; ++i) {
    if (layout[i].id != id) {
      continue;
    }
    SetTouchControlPlacement(id, layout[i].centre_x, layout[i].centre_y,
                             layout[i].radius * factor);
    return;
  }
}

void SetTouchLayoutReservedRect(float x0, float y0, float x1, float y1) {
  g_reserved_x0.store(x0, std::memory_order_relaxed);
  g_reserved_y0.store(y0, std::memory_order_relaxed);
  g_reserved_x1.store(x1, std::memory_order_relaxed);
  g_reserved_y1.store(y1, std::memory_order_relaxed);
}

TouchVisualState GetTouchVisualState() { return Sample(); }

bool TouchControlsActive() {
  return REXCVAR_GET(touch_controls) && !g_physical_controller.load(std::memory_order_relaxed);
}

void SetMenuButtonCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(g_menu_mutex);
  g_menu_callback = std::move(callback);
}

void SetPhysicalControllerConnected(bool connected) {
  const bool was = g_physical_controller.exchange(connected, std::memory_order_relaxed);
  if (was != connected) {
    REXLOG_WARN("touch: physical controller {}; on-screen controls {}",
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
