// =============================================================================
//  model.h - core data model: notes, CC curves, settings (shared, mutex-guarded)
// =============================================================================
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// A recorded MIDI note. Positions are stored as fixed-point ticks; see
// TICKS_PER_BAR/TICKS_PER_BEAT in config.h for the fractional-bar mapping.
struct Note {
  uint8_t  id = 0;
  uint8_t  pitch = 60;
  uint8_t  velocity = 100;
  uint32_t startTick = 0;
  uint32_t durationTick = TICKS_PER_BEAT;
  uint32_t endTick() const { return startTick + durationTick; }
};

struct CcPoint {
  uint32_t positionTick;
  uint8_t  value;
};

enum class CcInputMode : uint8_t { MIDI_IN, LOCAL };

struct CcCurve {
  uint8_t     ccNumber = 1;
  bool        muted = true;
  CcInputMode inputMode = CcInputMode::MIDI_IN;
  uint16_t    pointCount = 0;
  CcPoint     points[MAX_CC_POINTS];
};

// Quantization grid: OFF uses continuous positions; the others snap note
// starts to the nearest multiple of 1, 1/2, 1/4 or 1/8 of a beat.
enum class QuantMode : uint8_t { OFF, BEAT, HALF, QUARTER, EIGHTH, COUNT };

// Global loop settings. Defaults per description-v2.md section 21 (adapted).
struct Settings {
  uint8_t  bars             = 2;
  uint16_t bpm              = 120;
  uint8_t  ccRateHz         = 30;
  uint8_t  outputChannel    = 1;   // 1..16
  uint8_t  ccKnobSensitivity = 2;  // multiplies each encoder step for LOCAL CC input
};

namespace Model {

  // Guards notes[]/noteCount and ccCurves[] against concurrent access from the
  // MIDI task (core 0) and the UI/display tasks (core 1).
  extern SemaphoreHandle_t mutex;

  extern Note     notes[MAX_NOTES];
  extern uint16_t noteCount;

  extern CcCurve  ccCurves[CC_CURVE_COUNT];
  extern uint8_t  selectedCurve; // 0..CC_CURVE_COUNT-1

  extern Settings  settings;
  extern QuantMode quantMode;

  // Pitch of the last MIDI Note On received (used as default for new notes).
  extern volatile uint8_t lastInputPitch;

  void init();

  uint32_t loopLengthTicks();
  uint32_t clampToLoop(uint32_t tick);
  // Tick size of one quantization step for the given mode (0 for OFF).
  uint32_t quantStepTicks(QuantMode mode);
  // Rounds a tick position to the nearest step of the current quantMode
  // (returns it unchanged when quantMode is OFF). Stored note/CC positions
  // are never altered by this; it's only used for output/UI quantization.
  uint32_t quantizeTick(uint32_t tick);

  // --- Notes (caller must not hold mutex; these take it internally) ----------
  int16_t findIndexById(uint8_t id);
  int16_t findIndexByPitch(uint8_t pitch);
  // Adds a note, merging with an existing same-pitch note if one exists
  // (earliest start / latest end, velocity of the new Note On, ID preserved
  // from the pre-existing note). Returns the id of the resulting note, or
  // 255 if the note could not be stored (capacity reached).
  uint8_t addOrMergeNote(uint8_t pitch, uint8_t velocity, uint32_t startTick, uint32_t endTick);
  bool deleteNoteById(uint8_t id);
  void clearAllNotes();

  // Copies the note with the given id into `out`. Returns false if not found.
  bool getNoteCopy(uint8_t id, Note &out);

  // Sorted (ascending id) navigation helpers, -1 when there is no such note.
  int16_t firstNoteId();
  int16_t lastNoteId();
  int16_t nextNoteId(uint8_t id);
  int16_t prevNoteId(uint8_t id);

  // Note Edit page mutators.
  bool setNoteStart(uint8_t id, uint32_t newStart);
  bool setNoteDuration(uint8_t id, uint32_t newDuration);
  bool setNoteVelocity(uint8_t id, uint8_t newVelocity);
  // Changes pitch; merges with any other note that ends up sharing the same
  // pitch (keeping this note's id and velocity, widening start/end).
  void setNotePitch(uint8_t id, uint8_t newPitch);
  // Creates a new note (pitch = lastInputPitch, duration = one beat) starting
  // at `startTick`, merging with an existing same-pitch note if required.
  // Returns the resulting note's id, or 255 on failure (capacity reached).
  uint8_t createNoteAt(uint32_t startTick);

  // --- CC curves ---------------------------------------------------------
  void addOrUpdatePoint(uint8_t curveIdx, uint32_t positionTick, uint8_t value);
  void clearCurve(uint8_t curveIdx);
  uint8_t curveValueAt(uint8_t curveIdx, uint32_t positionTick);
  // Removes every point strictly after `tick` (used when a LOCAL CC
  // recording starts overriding the curve from the playhead onward).
  void erasePointsAfter(uint8_t curveIdx, uint32_t tick);
  int16_t curvePointCount(uint8_t curveIdx); // returns 0 if curveIdx is invalid

  // --- misc ----------------------------------------------------------------
  // Writes a short note name (e.g. "C#4") for the given MIDI pitch.
  void noteName(uint8_t pitch, char *buf, size_t len);

} // namespace Model
