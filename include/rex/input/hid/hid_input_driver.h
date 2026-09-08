#pragma once
/**
 * @file        rex/input/hid/hid_input_driver.h
 * @brief       The Switch pad, presented to the guest as an Xbox 360 pad.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <mutex>

#include <rex/input/input_driver.h>

namespace rex::input::hid {

class HidInputDriver final : public InputDriver {
 public:
  explicit HidInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~HidInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  // Sampled from the applet loop, which is the thread hid wants to be read
  // from. Everything else reads the snapshot this leaves behind.
  static void PumpFromUIThread();

 private:
  struct Snapshot {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
    uint32_t packet_number = 0;
  };

  static std::mutex snapshot_mutex_;
  static Snapshot snapshot_;
  static PadState pad_;
  static bool pad_ready_;

  // Two hands, and two ways the pad can be attached. A Joy-Con pair in
  // handheld mode and a Pro Controller are different npad identities, and
  // which one is live changes when the console is docked mid-session, so both
  // are opened up front and both are sent to.
  HidVibrationDeviceHandle vibration_handheld_[2] = {};
  HidVibrationDeviceHandle vibration_detached_[2] = {};
  bool vibration_handheld_ready_ = false;
  bool vibration_detached_ready_ = false;
};

}  // namespace rex::input::hid

#endif  // REX_PLATFORM_SWITCH
