/**
 * Unit tests for the controller button map (hid_button_map).
 *
 * The property that matters here is that a ButtonMap is ALWAYS a permutation.
 * That is what guarantees no guest button can end up driven by nothing, which
 * is in turn what makes the press-to-bind UI safe to ship without a
 * confirmation step: every rebind is undoable because every button stays
 * reachable. A hand-edited or corrupt settings.toml must not be able to break
 * it either, so the parser repairs rather than trusts.
 */

#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/input/button_map.h>

using namespace rex::input;

namespace {

bool IsPermutation(const ButtonMap& map) {
  std::set<int> seen;
  for (size_t i = 0; i < kPadInputCount; ++i) {
    if (size_t(map[i]) >= kPadInputCount) {
      return false;
    }
    if (!seen.insert(int(map[i])).second) {
      return false;
    }
  }
  return seen.size() == kPadInputCount;
}

}  // namespace

TEST_CASE("default button map is the console layout", "[input][button_map]") {
  const ButtonMap map = DefaultButtonMap();
  REQUIRE(IsPermutation(map));
  REQUIRE(ButtonMapIsDefault(map));
  // Empty, so SaveConfigValues drops the key from settings.toml entirely and
  // an install that never opened the Controls page is untouched.
  REQUIRE(FormatButtonMap(map).empty());
  REQUIRE(ButtonMapIsDefault(ParseButtonMap("")));
}

TEST_CASE("specs round-trip", "[input][button_map]") {
  const ButtonMap swapped = ParseButtonMap("a=b,b=a");
  REQUIRE(IsPermutation(swapped));
  REQUIRE(swapped[size_t(PadInput::kA)] == PadInput::kB);
  REQUIRE(swapped[size_t(PadInput::kB)] == PadInput::kA);
  REQUIRE(FormatButtonMap(swapped) == "a=b,b=a");
  REQUIRE(ParseButtonMap(FormatButtonMap(swapped)) == swapped);

  REQUIRE(ParseButtonMap("A=B") == swapped);
  REQUIRE(ParseButtonMap(" a = b ") == ParseButtonMap("a=b"));
  REQUIRE(ParseButtonMap("a=b;b=a") == swapped);
}

TEST_CASE("every token survives a round trip", "[input][button_map]") {
  for (size_t i = 0; i < kPadInputCount; ++i) {
    PadInput back{};
    REQUIRE(PadInputFromToken(PadInputToken(PadInput(i)), &back));
    REQUIRE(back == PadInput(i));
  }
  // The chord cvars accept these spellings; a settings.toml written by someone
  // reading that help text must not be rejected.
  PadInput out{};
  REQUIRE(PadInputFromToken("select", &out));
  REQUIRE(out == PadInput::kBack);
  REQUIRE(PadInputFromToken("menu", &out));
  REQUIRE(out == PadInput::kStart);
  REQUIRE_FALSE(PadInputFromToken("guide", &out));  // not remappable
}

TEST_CASE("a partial or malformed spec is repaired, never trusted",
          "[input][button_map]") {
  // "a=b" alone leaves physical A driving nothing unless the gap is repaired.
  const ButtonMap half = ParseButtonMap("a=b");
  REQUIRE(IsPermutation(half));
  REQUIRE(half[size_t(PadInput::kA)] == PadInput::kB);

  const char* junk[] = {"=",          ",,,",       "a=",        "=b",
                        "zzz=qqq",    "a=a",       "a=b,a=c,b=b", "a=b,b=c,c=a",
                        "lt=rt,rt=lt", "a=lt,lt=a", "menu=select,view=menu",
                        "dpad_up=start,start=dpad_up"};
  for (const char* spec : junk) {
    INFO("spec: " << spec);
    REQUIRE(IsPermutation(ParseButtonMap(spec)));
  }
}

TEST_CASE("assigning a source swaps rather than duplicating",
          "[input][button_map]") {
  ButtonMap map = DefaultButtonMap();
  ButtonMapAssign(map, PadInput::kA, PadInput::kB);
  REQUIRE(IsPermutation(map));
  REQUIRE(map[size_t(PadInput::kA)] == PadInput::kB);
  REQUIRE(map[size_t(PadInput::kB)] == PadInput::kA);

  ButtonMapAssign(map, PadInput::kA, PadInput::kA);
  REQUIRE(ButtonMapIsDefault(map));

  // Chained rebinds must not be able to break the invariant either.
  ButtonMapAssign(map, PadInput::kA, PadInput::kLT);
  ButtonMapAssign(map, PadInput::kX, PadInput::kA);
  ButtonMapAssign(map, PadInput::kDpadUp, PadInput::kStart);
  REQUIRE(IsPermutation(map));
}

TEST_CASE("applying the default map changes nothing", "[input][button_map]") {
  X_INPUT_GAMEPAD pad{};
  pad.buttons = uint16_t(X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_DPAD_LEFT |
                         X_INPUT_GAMEPAD_GUIDE);
  pad.left_trigger = 200;
  pad.right_trigger = 7;
  pad.thumb_lx = 1234;
  pad.thumb_ry = -4321;
  const X_INPUT_GAMEPAD before = pad;

  ApplyButtonMap(pad, DefaultButtonMap(), 0);

  REQUIRE(uint16_t(pad.buttons) == uint16_t(before.buttons));
  REQUIRE(pad.left_trigger == 200);
  REQUIRE(pad.right_trigger == 7);
  // Sticks are not remappable - Swap Sticks and Invert Camera Y own that.
  REQUIRE(int16_t(pad.thumb_lx) == 1234);
  REQUIRE(int16_t(pad.thumb_ry) == -4321);
}

TEST_CASE("a permutation does not feed itself", "[input][button_map]") {
  const ButtonMap swapped = ParseButtonMap("a=b,b=a");

  X_INPUT_GAMEPAD one{};
  one.buttons = X_INPUT_GAMEPAD_A;
  ApplyButtonMap(one, swapped, 0);
  REQUIRE(uint16_t(one.buttons) == X_INPUT_GAMEPAD_B);

  // Applied in place without reading a copy first, this would come out as one
  // button or none.
  X_INPUT_GAMEPAD both{};
  both.buttons = uint16_t(X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B);
  ApplyButtonMap(both, swapped, 0);
  REQUIRE(uint16_t(both.buttons) ==
          uint16_t(X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
}

TEST_CASE("triggers bind in both directions", "[input][button_map]") {
  ButtonMap to_button = DefaultButtonMap();
  ButtonMapAssign(to_button, PadInput::kA, PadInput::kLT);

  X_INPUT_GAMEPAD pulled{};
  pulled.left_trigger = 200;
  ApplyButtonMap(pulled, to_button, 0);
  REQUIRE((uint16_t(pulled.buttons) & X_INPUT_GAMEPAD_A) != 0);

  // A trigger resting slightly off zero on a worn pad must not read as a press.
  X_INPUT_GAMEPAD resting{};
  resting.left_trigger = 10;
  ApplyButtonMap(resting, to_button, 0);
  REQUIRE((uint16_t(resting.buttons) & X_INPUT_GAMEPAD_A) == 0);

  // hid_trigger_threshold, when the player has set one, is what decides.
  X_INPUT_GAMEPAD tuned{};
  tuned.left_trigger = 40;
  ApplyButtonMap(tuned, to_button, 200);
  REQUIRE((uint16_t(tuned.buttons) & X_INPUT_GAMEPAD_A) == 0);

  ButtonMap to_trigger = DefaultButtonMap();
  ButtonMapAssign(to_trigger, PadInput::kLT, PadInput::kA);
  X_INPUT_GAMEPAD held{};
  held.buttons = X_INPUT_GAMEPAD_A;
  ApplyButtonMap(held, to_trigger, 0);
  REQUIRE(held.left_trigger == 255);
}

TEST_CASE("Guide is never remapped away", "[input][button_map]") {
  // Guide is not a gameplay button - guide_button and picker_chord own it -
  // so it passes through rather than being dropped by the remap loop.
  X_INPUT_GAMEPAD pad{};
  pad.buttons = X_INPUT_GAMEPAD_GUIDE;
  ApplyButtonMap(pad, ParseButtonMap("a=b,b=a"), 0);
  REQUIRE((uint16_t(pad.buttons) & X_INPUT_GAMEPAD_GUIDE) != 0);
}
