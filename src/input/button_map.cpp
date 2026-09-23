/**
 * ReXGlue - user-configurable controller button mapping.
 */

#include <rex/input/button_map.h>

#include <algorithm>
#include <cctype>

namespace rex::input {
namespace {

struct PadInputInfo {
  PadInput input;
  const char* token;
  const char* label;
  uint16_t bit;
};

// Order matches the PadInput enum, and the settings rows are built in this
// order too - face buttons, shoulders, triggers, sticks, system, d-pad - so
// the list reads the way a pad is held rather than the way the bits happen to
// be numbered.
constexpr std::array<PadInputInfo, kPadInputCount> kPadInputs = {{
    {PadInput::kA, "a", "A", X_INPUT_GAMEPAD_A},
    {PadInput::kB, "b", "B", X_INPUT_GAMEPAD_B},
    {PadInput::kX, "x", "X", X_INPUT_GAMEPAD_X},
    {PadInput::kY, "y", "Y", X_INPUT_GAMEPAD_Y},
    {PadInput::kLB, "lb", "Left Bumper", X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {PadInput::kRB, "rb", "Right Bumper", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {PadInput::kLT, "lt", "Left Trigger", 0},
    {PadInput::kRT, "rt", "Right Trigger", 0},
    {PadInput::kL3, "l3", "Left Stick Click", X_INPUT_GAMEPAD_LEFT_THUMB},
    {PadInput::kR3, "r3", "Right Stick Click", X_INPUT_GAMEPAD_RIGHT_THUMB},
    {PadInput::kBack, "back", "Back", X_INPUT_GAMEPAD_BACK},
    {PadInput::kStart, "start", "Start", X_INPUT_GAMEPAD_START},
    {PadInput::kDpadUp, "dpad_up", "D-Pad Up", X_INPUT_GAMEPAD_DPAD_UP},
    {PadInput::kDpadDown, "dpad_down", "D-Pad Down", X_INPUT_GAMEPAD_DPAD_DOWN},
    {PadInput::kDpadLeft, "dpad_left", "D-Pad Left", X_INPUT_GAMEPAD_DPAD_LEFT},
    {PadInput::kDpadRight, "dpad_right", "D-Pad Right", X_INPUT_GAMEPAD_DPAD_RIGHT},
}};

// A trigger driving a digital button still has to decide when it counts as
// pressed. hid_trigger_threshold is the player's own answer when they have set
// one; this is the fallback, low enough that a light pull registers and high
// enough that a resting trigger on a worn pad does not.
constexpr uint8_t kTriggerDigitalFloor = 48;

}  // namespace

const char* PadInputToken(PadInput input) {
  const size_t index = size_t(input);
  return index < kPadInputCount ? kPadInputs[index].token : "";
}

const char* PadInputLabel(PadInput input) {
  const size_t index = size_t(input);
  return index < kPadInputCount ? kPadInputs[index].label : "";
}

uint16_t PadInputBit(PadInput input) {
  const size_t index = size_t(input);
  return index < kPadInputCount ? kPadInputs[index].bit : uint16_t(0);
}

bool PadInputFromToken(std::string_view token, PadInput* out_input) {
  for (const auto& info : kPadInputs) {
    if (token == info.token) {
      if (out_input) {
        *out_input = info.input;
      }
      return true;
    }
  }
  // The chord grammar accepts more than one spelling for these two, and a
  // settings.toml hand-edited by someone reading that cvar's help text should
  // not be rejected here.
  if (token == "select" || token == "view") {
    if (out_input) *out_input = PadInput::kBack;
    return true;
  }
  if (token == "menu") {
    if (out_input) *out_input = PadInput::kStart;
    return true;
  }
  return false;
}

ButtonMap DefaultButtonMap() {
  ButtonMap map{};
  for (size_t i = 0; i < kPadInputCount; ++i) {
    map[i] = PadInput(i);
  }
  return map;
}

bool ButtonMapIsDefault(const ButtonMap& map) {
  for (size_t i = 0; i < kPadInputCount; ++i) {
    if (map[i] != PadInput(i)) {
      return false;
    }
  }
  return true;
}

void ButtonMapAssign(ButtonMap& map, PadInput guest, PadInput source) {
  const size_t guest_index = size_t(guest);
  if (guest_index >= kPadInputCount || size_t(source) >= kPadInputCount) {
    return;
  }
  const PadInput previous = map[guest_index];
  if (previous == source) {
    return;
  }
  // Swap with whoever held this source. Without this the map stops being a
  // permutation and some guest button ends up driven by nothing at all.
  for (size_t i = 0; i < kPadInputCount; ++i) {
    if (map[i] == source) {
      map[i] = previous;
      break;
    }
  }
  map[guest_index] = source;
}

ButtonMap ParseButtonMap(std::string_view spec) {
  ButtonMap map = DefaultButtonMap();
  std::array<bool, kPadInputCount> guest_seen{};
  std::array<bool, kPadInputCount> source_taken{};

  std::string guest_token;
  std::string source_token;
  bool in_source = false;

  auto flush = [&]() {
    PadInput guest{};
    PadInput source{};
    if (!guest_token.empty() && !source_token.empty() &&
        PadInputFromToken(guest_token, &guest) &&
        PadInputFromToken(source_token, &source)) {
      // First writer wins for both sides. A malformed or hostile string can
      // then only ever produce a partial map, never a broken one: anything it
      // does not name stays identity.
      if (!guest_seen[size_t(guest)] && !source_taken[size_t(source)]) {
        guest_seen[size_t(guest)] = true;
        source_taken[size_t(source)] = true;
        map[size_t(guest)] = source;
      }
    }
    guest_token.clear();
    source_token.clear();
    in_source = false;
  };

  for (size_t i = 0; i <= spec.size(); ++i) {
    const char c = i < spec.size() ? spec[i] : ',';
    if (c == ',' || c == ';') {
      flush();
      continue;
    }
    if (c == '=' || c == ':') {
      in_source = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      continue;
    }
    const char lowered = char(std::tolower(static_cast<unsigned char>(c)));
    (in_source ? source_token : guest_token).push_back(lowered);
  }

  // The entries that were named may have consumed a source that an unnamed
  // guest still holds by identity, which would leave that source driving two
  // guest buttons. Repair by giving every unclaimed guest an unclaimed source.
  for (size_t i = 0; i < kPadInputCount; ++i) {
    if (guest_seen[i]) {
      continue;
    }
    if (!source_taken[size_t(map[i])]) {
      source_taken[size_t(map[i])] = true;
      continue;
    }
    map[i] = PadInput::kCount;  // parked; filled in below
  }
  for (size_t i = 0; i < kPadInputCount; ++i) {
    if (map[i] != PadInput::kCount) {
      continue;
    }
    for (size_t j = 0; j < kPadInputCount; ++j) {
      if (!source_taken[j]) {
        source_taken[j] = true;
        map[i] = PadInput(j);
        break;
      }
    }
  }
  return map;
}

std::string FormatButtonMap(const ButtonMap& map) {
  std::string out;
  for (size_t i = 0; i < kPadInputCount; ++i) {
    if (map[i] == PadInput(i)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += PadInputToken(PadInput(i));
    out.push_back('=');
    out += PadInputToken(map[i]);
  }
  return out;
}

void ApplyButtonMap(X_INPUT_GAMEPAD& pad, const ButtonMap& map, uint32_t trigger_threshold) {
  // Read everything out first. A permutation applied in place would feed its
  // own output back in halfway through.
  const uint16_t in_buttons = static_cast<uint16_t>(pad.buttons);
  const uint8_t in_left_trigger = pad.left_trigger;
  const uint8_t in_right_trigger = pad.right_trigger;

  const uint8_t digital_floor =
      trigger_threshold > 0 ? uint8_t(std::min<uint32_t>(trigger_threshold, 255))
                            : kTriggerDigitalFloor;

  auto source_pressed = [&](PadInput source) -> bool {
    switch (source) {
      case PadInput::kLT:
        return in_left_trigger >= digital_floor;
      case PadInput::kRT:
        return in_right_trigger >= digital_floor;
      default:
        return (in_buttons & PadInputBit(source)) != 0;
    }
  };
  auto source_value = [&](PadInput source) -> uint8_t {
    switch (source) {
      case PadInput::kLT:
        return in_left_trigger;
      case PadInput::kRT:
        return in_right_trigger;
      default:
        return (in_buttons & PadInputBit(source)) != 0 ? uint8_t(255) : uint8_t(0);
    }
  };

  uint16_t out_buttons = 0;
  for (size_t i = 0; i < kPadInputCount; ++i) {
    const PadInput guest = PadInput(i);
    if (PadInputIsTrigger(guest)) {
      continue;
    }
    if (source_pressed(map[i])) {
      out_buttons |= PadInputBit(guest);
    }
  }
  // Guide is not remappable, so it passes straight through rather than being
  // dropped on the floor by the loop above.
  out_buttons |= uint16_t(in_buttons & X_INPUT_GAMEPAD_GUIDE);

  pad.buttons = out_buttons;
  pad.left_trigger = source_value(map[size_t(PadInput::kLT)]);
  pad.right_trigger = source_value(map[size_t(PadInput::kRT)]);
}

}  // namespace rex::input
