// =============================================================================
//  midi_engine.h - USB MIDI I/O, recording capture, note/CC playback scheduling
// =============================================================================
#pragma once
#include <Arduino.h>
#include "model.h"

namespace MidiEngine {

  // Starts the MIDI task pinned to core 0 at high priority. Must be called
  // once from setup(), after Model::init()/Transport::init().
  void start();

  // --- State exposed for the UI / display (read-only from other tasks) -------

  // True while a MIDI Note On for this pitch has been received and no
  // matching Note Off has arrived yet (drives the right-column indicator).
  extern volatile bool activeInputNote[128];

  // Last MIDI CC value received per curve, for the CC Edit page's MIDI IN
  // input-column display.
  extern volatile uint8_t lastCcInputValue[CC_CURVE_COUNT];

  // Current value driven by the rotary encoder while recording a LOCAL CC
  // curve (either "following" the existing curve, or overriding it).
  extern volatile uint8_t localCcValue;
  // extern volatile bool    localCcFollowing;

  // Called by the UI when the encoder is rotated while a LOCAL CC recording
  // session is active on the CC Edit page (overrides normal field navigation).
  void applyLocalCcDelta(int16_t delta);

  // Called by the UI right after toggling the CC Play/Record button, so a
  // LOCAL-mode recording session starts by following the existing curve
  // value until the encoder is first rotated.
  void notifyCcRecordStarted();

  // Requests a short audition Note On/Off for the given pitch/velocity.
  // Used by the Note Edit page while the transport is stopped, per
  // description-v2.md ("every edition ... triggers a midi output").
  void requestNotePreview(uint8_t pitch, uint8_t velocity);

} // namespace MidiEngine
