// =============================================================================
//  input.h - rotary encoder + physical button reading
// =============================================================================
#pragma once
#include <Arduino.h>

namespace Input {

  // Configures pins and attaches the encoder quadrature interrupt.
  void init();

  // Returns accumulated encoder ticks since the last call (+CW / -CCW),
  // then resets the internal counter to zero.
  int16_t consumeEncoderDelta();

  // Debounced "just pressed" edge detectors; call once per poll cycle.
  bool encoderButtonPressed();
  bool runStopButtonPressed();

} // namespace Input
