/**
 * @file        input/hid/hid_input_driver.cpp
 * @brief       The Switch pad, presented to the guest as an Xbox 360 pad.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/input/hid/hid_input_driver.h>

#include <algorithm>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app_context_switch.h>

REXCVAR_DEFINE_BOOL(
    switch_pad_positional, true, "Input",
    "Map the face buttons by position rather than by letter. Nintendo and "
    "Microsoft swap both pairs: the button at the bottom is B on a Switch pad "
    "and A on an Xbox pad. Mapping by position means the button the game's own "
    "prompts point at is the one in the same place it would be on the "
    "controller the game was written for, which is what muscle memory follows. "
    "Turn this off to match the printed letters instead.");

namespace rex::input::hid {

std::mutex HidInputDriver::snapshot_mutex_;
HidInputDriver::Snapshot HidInputDriver::snapshot_;
PadState HidInputDriver::pad_;
bool HidInputDriver::pad_ready_ = false;

namespace {

// The guest's sticks are already the same range and sign convention as the
// console's, so only the vertical axis needs anything: both point up-positive,
// which is what XInput expects, so nothing is inverted here.
int16_t ClampStick(int32_t value) {
  return int16_t(std::clamp(value, -32767, 32767));
}

}  // namespace

HidInputDriver::HidInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

HidInputDriver::~HidInputDriver() = default;

X_STATUS HidInputDriver::Setup() {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad_);
  pad_ready_ = true;

  // Rumble is per hand, and the pad's identity depends on how it is attached:
  // Joy-Cons in handheld mode are a different npad from a Pro Controller or a
  // detached pair. Which is live can change mid-session when the console is
  // docked, so both are opened now and both are sent to later.
  vibration_handheld_ready_ = R_SUCCEEDED(hidInitializeVibrationDevices(
      vibration_handheld_, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
  vibration_detached_ready_ = R_SUCCEEDED(hidInitializeVibrationDevices(
      vibration_detached_, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual));
  if (!vibration_handheld_ready_ && !vibration_detached_ready_) {
    REXLOG_WARN("HidInputDriver: no vibration devices; rumble will be ignored");
  }

  // The applet loop owns the thread hid is read from, so it calls back here.
  rex::ui::SwitchWindowedAppContext::SetInputPump(&HidInputDriver::PumpFromUIThread);
  return X_STATUS_SUCCESS;
}

void HidInputDriver::PumpFromUIThread() {
  if (!pad_ready_) {
    return;
  }
  padUpdate(&pad_);

  const u64 buttons = padGetButtons(&pad_);
  const HidAnalogStickState left = padGetStickPos(&pad_, 0);
  const HidAnalogStickState right = padGetStickPos(&pad_, 1);
  const bool positional = REXCVAR_GET(switch_pad_positional);

  Snapshot next;

  // The two shoulder buttons map straight across. ZL and ZR are the analogue
  // triggers on an Xbox pad but digital here, so they report fully pressed or
  // not at all - the guest only ever compares them against a threshold.
  if (buttons & HidNpadButton_L) next.buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (buttons & HidNpadButton_R) next.buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  next.left_trigger = (buttons & HidNpadButton_ZL) ? 255 : 0;
  next.right_trigger = (buttons & HidNpadButton_ZR) ? 255 : 0;

  if (buttons & HidNpadButton_Up) next.buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (buttons & HidNpadButton_Down) next.buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (buttons & HidNpadButton_Left) next.buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (buttons & HidNpadButton_Right) next.buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  if (buttons & HidNpadButton_Plus) next.buttons |= X_INPUT_GAMEPAD_START;
  if (buttons & HidNpadButton_Minus) next.buttons |= X_INPUT_GAMEPAD_BACK;
  if (buttons & HidNpadButton_StickL) next.buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (buttons & HidNpadButton_StickR) next.buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;

  // The face buttons are the only genuinely ambiguous part. libnx names them
  // by their position on the pad: HidNpadButton_A is the one on the right,
  // HidNpadButton_B the one at the bottom. On an Xbox pad those positions are
  // B and A respectively.
  if (positional) {
    if (buttons & HidNpadButton_B) next.buttons |= X_INPUT_GAMEPAD_A;  // bottom
    if (buttons & HidNpadButton_A) next.buttons |= X_INPUT_GAMEPAD_B;  // right
    if (buttons & HidNpadButton_Y) next.buttons |= X_INPUT_GAMEPAD_X;  // left
    if (buttons & HidNpadButton_X) next.buttons |= X_INPUT_GAMEPAD_Y;  // top
  } else {
    if (buttons & HidNpadButton_A) next.buttons |= X_INPUT_GAMEPAD_A;
    if (buttons & HidNpadButton_B) next.buttons |= X_INPUT_GAMEPAD_B;
    if (buttons & HidNpadButton_X) next.buttons |= X_INPUT_GAMEPAD_X;
    if (buttons & HidNpadButton_Y) next.buttons |= X_INPUT_GAMEPAD_Y;
  }

  next.thumb_lx = ClampStick(left.x);
  next.thumb_ly = ClampStick(left.y);
  next.thumb_rx = ClampStick(right.x);
  next.thumb_ry = ClampStick(right.y);

  std::lock_guard<std::mutex> guard(snapshot_mutex_);
  // The guest polls for changes by comparing packet numbers, so this only
  // advances when something actually moved.
  const bool changed = next.buttons != snapshot_.buttons ||
                       next.left_trigger != snapshot_.left_trigger ||
                       next.right_trigger != snapshot_.right_trigger ||
                       next.thumb_lx != snapshot_.thumb_lx ||
                       next.thumb_ly != snapshot_.thumb_ly ||
                       next.thumb_rx != snapshot_.thumb_rx ||
                       next.thumb_ry != snapshot_.thumb_ry;
  next.packet_number = changed ? snapshot_.packet_number + 1 : snapshot_.packet_number;
  snapshot_ = next;
}

X_RESULT HidInputDriver::GetCapabilities(uint32_t user_index, uint32_t /*flags*/,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;     // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = 0;
  // Every field the pad can report, which is all of them.
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(0xFFFFu);
  out_caps->gamepad.thumb_ly = int16_t(0xFFFFu);
  out_caps->gamepad.thumb_rx = int16_t(0xFFFFu);
  out_caps->gamepad.thumb_ry = int16_t(0xFFFFu);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT HidInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard<std::mutex> guard(snapshot_mutex_);
  out_state->packet_number = snapshot_.packet_number;
  out_state->gamepad.buttons = snapshot_.buttons;
  out_state->gamepad.left_trigger = snapshot_.left_trigger;
  out_state->gamepad.right_trigger = snapshot_.right_trigger;
  out_state->gamepad.thumb_lx = snapshot_.thumb_lx;
  out_state->gamepad.thumb_ly = snapshot_.thumb_ly;
  out_state->gamepad.thumb_rx = snapshot_.thumb_rx;
  out_state->gamepad.thumb_ry = snapshot_.thumb_ry;
  return X_ERROR_SUCCESS;
}

X_RESULT HidInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!vibration_handheld_ready_ && !vibration_detached_ready_) {
    return X_ERROR_SUCCESS;
  }

  // The guest speaks in two motor speeds; the console wants an amplitude at a
  // low and a high frequency per hand. The 360's heavy motor lives on the left
  // and its light one on the right, which is the same split as the two bands.
  const float left = float(uint16_t(vibration->left_motor_speed)) / 65535.0f;
  const float right = float(uint16_t(vibration->right_motor_speed)) / 65535.0f;

  HidVibrationValue values[2] = {};
  // The frequencies are the ones libnx's own samples use: low enough to be felt
  // as a rumble and high enough to be felt as a buzz.
  values[0].freq_low = 160.0f;
  values[0].freq_high = 320.0f;
  values[0].amp_low = left;
  values[0].amp_high = right;
  values[1] = values[0];

  // Sent to both identities: the one that is not currently attached ignores it,
  // which is cheaper than tracking dock transitions to decide.
  if (vibration_handheld_ready_) {
    hidSendVibrationValues(vibration_handheld_, values, 2);
  }
  if (vibration_detached_ready_) {
    hidSendVibrationValues(vibration_detached_, values, 2);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT HidInputDriver::GetKeystroke(uint32_t /*user_index*/, uint32_t /*flags*/,
                                      X_INPUT_KEYSTROKE* /*out_keystroke*/) {
  // Only the on-screen keyboard produces these, and the guest reaches that
  // through the XAM path rather than here.
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::hid

#endif  // REX_PLATFORM_SWITCH
