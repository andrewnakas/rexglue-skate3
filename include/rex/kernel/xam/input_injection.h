#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::kernel::xam {

struct SyntheticInputStep {
  uint16_t buttons;
  uint32_t poll_count;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  uint32_t marker = 0;
};

using SyntheticInputMarkerCallback = void (*)(uint32_t marker);

struct SyntheticInputTelemetry {
  bool queue_active = false;
  bool hold_active = false;
  bool replace_physical_input = false;
  size_t step_count = 0;
  size_t step_index = 0;
  uint32_t step_polls_remaining = 0;
  uint64_t sequence_id = 0;
  uint64_t completed_sequence_id = 0;
  uint64_t applied_poll_count = 0;
};

void QueueSyntheticInput(uint16_t buttons, uint32_t poll_count);
void QueueSyntheticInput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                         uint32_t poll_count);
void QueueSyntheticInputSequence(const SyntheticInputStep* steps, size_t step_count);
// When replace_physical_input is true, synthetic input becomes the complete
// controller state rather than being merged with a physical controller.
void SetSyntheticInputMode(bool replace_physical_input);
// Holds a controller state until another state is supplied or synthetic input
// is cleared. This is intended for automation clients that run independently
// of host-window focus.
void SetSyntheticInputState(const SyntheticInputStep& state);
SyntheticInputTelemetry GetSyntheticInputTelemetry();
void SetSyntheticInputMarkerCallback(SyntheticInputMarkerCallback callback);
void SetSyntheticAutoTap(uint16_t buttons, bool enabled);
void ClearSyntheticInput();

}  // namespace rex::kernel::xam
