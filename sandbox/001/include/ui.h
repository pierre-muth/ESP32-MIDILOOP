// =============================================================================
//  ui.h  –  UI model: field focus, mode switching, encoder/button logic
// =============================================================================
#pragma once
#include "looper.h"

// ── UI field identifiers ──────────────────────────────────────────────────────
// Each screen has its own field list.  The first field of every screen is
// always the [Mode] selector.

// NOTE_LOOP fields
enum class NLField : uint8_t {
  MODE = 0, TRANSPORT, RECORDING, QUANTISE, BPM, BARS, _COUNT
};

// NOTE_EDIT fields
enum class NEField : uint8_t {
  MODE = 0, NOTE_SEL, NOTE_PITCH, NOTE_START, NOTE_DURATION,
  NOTE_VEL, QUANTISE, NEW_NOTE, DELETE_NOTE, _COUNT
};

// CC_LOOP fields
enum class CCField : uint8_t {
  MODE = 0, TRANSPORT, CURVE, RECORDING, INPUT_MODE,
  BPM, MUTE, CC_NUMBER, CLEAR_CURVE, BARS, _COUNT
};

// SETTINGS fields
enum class SField : uint8_t {
  MODE = 0, BARS, BPM, CC_RATE, OUTPUT_CHANNEL, _COUNT
};

// ── UI state ──────────────────────────────────────────────────────────────────
namespace UI {

  // Currently focused field index within the active screen (0 = MODE field)
  extern uint8_t  focusIdx;
  // true while the focused field is being edited (encoder changes value)
  extern bool     editing;
  // Selected note index in NOTE_EDIT mode (-1 = none)
  extern int16_t  selectedNote;

  // Call once from setup()
  void init();

  // Call from loop() every ~10 ms.  Consumes encoder delta and button presses.
  // encoderDelta: accumulated ticks since last call (positive = CW)
  // encBtnPressed / recBtnPressed: true for one call only when pressed
  void update(int16_t encoderDelta, bool encBtnPressed, bool recBtnPressed);

  // Number of fields for the current mode
  uint8_t fieldCount();

  // Display label for a field (e.g. "SPD", "Q:16", "LOOP")
  // buf must be at least 10 bytes
  void fieldLabel(uint8_t fieldIdx, char *buf, size_t bufLen);

  // Is a given field index currently focused?
  inline bool isFocused(uint8_t idx) { return idx == focusIdx; }

} // namespace UI
