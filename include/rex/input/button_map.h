#pragma once
/**
 * ReXGlue - user-configurable controller button mapping.
 *
 * The console layout is the identity map, and that is the default: an install
 * that never opens the Controls page is bit-for-bit unaffected by any of this.
 */

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/input/input.h>

namespace rex::input {

// Every remappable input, used both as "the guest button being driven" and as
// "the physical control driving it". Guide is deliberately absent: it is a
// system button governed by guide_button and picker_chord, not a gameplay one.
enum class PadInput : uint8_t {
  kA,
  kB,
  kX,
  kY,
  kLB,
  kRB,
  kLT,
  kRT,
  kL3,
  kR3,
  kBack,
  kStart,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kCount,
};

constexpr size_t kPadInputCount = size_t(PadInput::kCount);

// map[guest] = the physical input that drives it. Always a permutation, which
// is what guarantees no guest button can become unreachable: binding a source
// that is already in use swaps the two rather than duplicating one.
using ButtonMap = std::array<PadInput, kPadInputCount>;

// The token as it appears in the hid_button_map cvar. Same vocabulary as the
// chord cvars (menu_chord and friends), plus "lt"/"rt", which a chord cannot
// express because it matches against a 16-bit button word.
const char* PadInputToken(PadInput input);

// Human-readable name for the settings rows: "A", "Left Trigger", "D-Pad Up".
const char* PadInputLabel(PadInput input);

bool PadInputFromToken(std::string_view token, PadInput* out_input);

// The two analog inputs. A digital source driving one reports 0 or 255; a
// trigger source driving a digital guest button is compared against a
// threshold.
constexpr bool PadInputIsTrigger(PadInput input) {
  return input == PadInput::kLT || input == PadInput::kRT;
}

// The X_INPUT_GAMEPAD_* bit, or 0 for the triggers.
uint16_t PadInputBit(PadInput input);

ButtonMap DefaultButtonMap();

// "a=b,b=a" -> map. Unparseable or missing entries stay identity, and an entry
// naming a source already claimed by an earlier entry is dropped rather than
// allowed to break the permutation - a hand-edited settings.toml must not be
// able to make a button unreachable.
ButtonMap ParseButtonMap(std::string_view spec);

// Only the entries that differ from identity, so a default map writes "" and
// SaveConfigValues then drops the key entirely.
std::string FormatButtonMap(const ButtonMap& map);

bool ButtonMapIsDefault(const ButtonMap& map);

// Rebind one guest input, swapping with whoever held that source. Keeps the
// map a permutation.
void ButtonMapAssign(ButtonMap& map, PadInput guest, PadInput source);

// Rewrite the pad in place. Reads a copy first, so a permutation cannot feed
// itself. trigger_threshold is hid_trigger_threshold; 0 means "use the small
// built-in floor", since a remapped trigger still has to decide when a digital
// guest button counts as pressed.
void ApplyButtonMap(X_INPUT_GAMEPAD& pad, const ButtonMap& map, uint32_t trigger_threshold);

}  // namespace rex::input
