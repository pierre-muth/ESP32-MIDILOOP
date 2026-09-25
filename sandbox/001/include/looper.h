// =============================================================================
//  looper.h  –  shared data model, timing, event buffers, settings
// =============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

// ── Capacities and musical resolution ────────────────────────────────────────
static const uint16_t MAX_NOTES       = 128;
static const uint8_t  CC_CURVE_COUNT  = 4;
static const uint16_t MAX_CC_POINTS   = 256;
static const uint32_t TICKS_PER_BEAT = 960;
static const uint32_t TICKS_PER_BAR  = TICKS_PER_BEAT * 4;

// ── Debug serial ──────────────────────────────────────────────────────────────
#define DBG Serial1
#define DBG_PRINTLN(x)   DBG.println(x)
#define DBG_PRINTF(...)  DBG.printf(__VA_ARGS__)

// ── Modes ─────────────────────────────────────────────────────────────────────
enum class AppMode : uint8_t {
  NOTE_LOOP = 0,
  NOTE_EDIT,
  CC_LOOP,
  SETTINGS,
  _COUNT
};

// ── Transport and recording state ────────────────────────────────────────────
enum class TransportState : uint8_t {
  STOPPED,
  RUNNING,
};

enum class RecordingState : uint8_t {
  PLAY,
  RECORD,
  ARMED,
};

enum class CcInputMode : uint8_t {
  MIDI_IN,
  LOCAL,
};

// ── Note event ────────────────────────────────────────────────────────────────
struct Note {
  uint8_t  pitch;
  uint8_t  velocity;
  uint32_t startTick;
  uint32_t durationTick;
};

struct CcPoint {
  uint32_t positionTick;
  uint8_t  value;
};

struct CcCurve {
  uint8_t    ccNumber = 1;
  bool       muted = true;
  CcInputMode inputMode = CcInputMode::MIDI_IN;
  uint8_t    localValue = 64;
  uint8_t    lastInputValue = 0;
  volatile bool localDirty = false;
  uint16_t   pointCount = 0;
  CcPoint    points[MAX_CC_POINTS];
};

// ── Quantise ──────────────────────────────────────────────────────────────────
enum class QuantDiv : uint8_t {
  OFF = 0,
  BAR,
  _COUNT
};

// ── Settings (persisted in future; for now RAM-only) ─────────────────────────
struct Settings {
  uint16_t  bpm           = 120;
  uint8_t   bars          = 8;
  uint8_t   ccRateHz      = 50;
  uint8_t   outputChannel = 0;      // 0-based, generated MIDI output channel
  QuantDiv  quantDiv      = QuantDiv::OFF;
  bool      showGrid      = true;
  bool      debugOutput   = true;
};

// ── Global shared state ───────────────────────────────────────────────────────
namespace Looper {

  extern volatile TransportState transport;
  extern volatile RecordingState recording;
  extern volatile AppMode        appMode;

  // Timing
  extern volatile uint64_t loopStartUs;
  extern volatile uint32_t loopDurationUs;
  extern volatile uint32_t stopTick;

  // Note buffer (shared, protected by mutex)
  extern Note notes[MAX_NOTES];
  extern uint16_t noteCount;
  extern volatile uint8_t lastInputPitch;

  // CC slots (shared, protected by mutex)
  extern CcCurve ccCurves[CC_CURVE_COUNT];
  extern uint8_t activeCcCurve;

  // Settings
  extern Settings  settings;

  // Mutex protecting notes[], noteCount, ccCurves[]
  extern SemaphoreHandle_t mutex;

  // ── Helpers ────────────────────────────────────────────────────────────────

  // Derive loop duration from BPM + bars.
  inline uint32_t bpmToLoopUs(uint16_t bpm, uint8_t bars) {
    if (bpm == 0) return 1000000UL;
    uint64_t beatUs = (uint64_t)60 * 1000000UL / bpm;
    return (uint32_t)(beatUs * 4 * bars);
  }

  uint32_t currentTick();
  uint32_t tickAtUs(uint64_t elapsedUs);
  uint32_t clampTick(uint32_t tick);
  uint8_t noteIndexForPitch(uint8_t pitch);
  void updateLoopDuration();
  void setBars(int bars);
  void setBpm(int bpm);
  void stop();
  void startRunning();
  void clearNotes();
  void clearCurve(uint8_t curve);
  void addOrMergeNote(uint8_t pitch, uint8_t velocity,
                      uint32_t startTick, uint32_t endTick);
  void addOrUpdatePoint(uint8_t curve, uint32_t positionTick, uint8_t value);
  uint8_t curveValueAt(const CcCurve &curve, uint32_t positionTick);

  // Initialise shared state (call once from setup before creating tasks)
  void init();

} // namespace Looper
