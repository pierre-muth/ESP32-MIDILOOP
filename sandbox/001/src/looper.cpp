// =============================================================================
//  looper.cpp  -  musical loop model, timing and recording data operations
// =============================================================================
#include "looper.h"
#include <algorithm>
#include <string.h>

namespace Looper {

volatile TransportState transport = TransportState::STOPPED;
volatile RecordingState recording = RecordingState::PLAY;
volatile AppMode appMode = AppMode::NOTE_LOOP;
volatile uint64_t loopStartUs = 0;
volatile uint32_t loopDurationUs = 0;
volatile uint32_t stopTick = 0;

Note notes[MAX_NOTES];
uint16_t noteCount = 0;
volatile uint8_t lastInputPitch = 60;
CcCurve ccCurves[CC_CURVE_COUNT];
uint8_t activeCcCurve = 0;
Settings settings;
SemaphoreHandle_t mutex = nullptr;

static uint32_t loopTicks() {
  return (uint32_t)settings.bars * TICKS_PER_BAR;
}

uint32_t clampTick(uint32_t tick) {
  uint32_t length = loopTicks();
  return length == 0 ? 0 : (tick >= length ? length - 1 : tick);
}

uint32_t tickAtUs(uint64_t elapsedUs) {
  uint32_t length = loopTicks();
  if (loopDurationUs == 0 || length == 0) return 0;
  return (uint32_t)((elapsedUs * length / loopDurationUs) % length);
}

uint32_t currentTick() {
  if (transport != TransportState::RUNNING) return 0;
  return tickAtUs(esp_timer_get_time() - loopStartUs);
}

void updateLoopDuration() {
  loopDurationUs = bpmToLoopUs(settings.bpm, settings.bars);
}

void setBars(int bars) {
  uint32_t oldLength = (uint32_t)settings.bars * TICKS_PER_BAR;
  uint32_t oldTick = currentTick();
  settings.bars = (uint8_t)constrain((int)bars, 1, 32);
  updateLoopDuration();
  if (transport == TransportState::RUNNING && oldLength != 0) {
    uint32_t newLength = (uint32_t)settings.bars * TICKS_PER_BAR;
    oldTick = (uint32_t)((uint64_t)oldTick * newLength / oldLength);
    loopStartUs = esp_timer_get_time() -
                  (uint64_t)oldTick * loopDurationUs / newLength;
  }
}

void setBpm(int bpm) {
  bpm = constrain(bpm, 20, 300);
  if (recording == RecordingState::RECORD) return;
  uint32_t oldTick = currentTick();
  settings.bpm = bpm;
  updateLoopDuration();
  if (transport == TransportState::RUNNING) {
    loopStartUs = esp_timer_get_time() -
                  (uint64_t)oldTick * loopDurationUs / loopTicks();
  }
}

void stop() {
  stopTick = currentTick();
  transport = TransportState::STOPPED;
  recording = RecordingState::PLAY;
  loopStartUs = 0;
}

void startRunning() {
  updateLoopDuration();
  loopStartUs = esp_timer_get_time();
  transport = TransportState::RUNNING;
}

uint8_t noteIndexForPitch(uint8_t pitch) {
  for (uint16_t i = 0; i < noteCount; i++)
    if (notes[i].pitch == pitch) return (uint8_t)i;
  return 255;
}

void addOrMergeNote(uint8_t pitch, uint8_t velocity,
                    uint32_t startTick, uint32_t endTick) {
  if (startTick >= endTick) endTick = startTick + 1;
  uint32_t length = (uint32_t)settings.bars * TICKS_PER_BAR;
  if (length == 0) return;
  startTick = clampTick(startTick);
  endTick = std::min(endTick, length);

  if (xSemaphoreTake(mutex, 0) != pdTRUE) return;
  uint32_t mergedStart = startTick;
  uint32_t mergedEnd = endTick;
  bool merged = false;
  for (uint16_t i = 0; i < noteCount;) {
    uint32_t noteEnd = notes[i].startTick + notes[i].durationTick;
    if (notes[i].pitch == pitch) {
      mergedStart = std::min(mergedStart, notes[i].startTick);
      mergedEnd = std::max(mergedEnd, noteEnd);
      notes[i] = notes[noteCount - 1];
      noteCount--;
      merged = true;
      continue;
    }
    i++;
  }
  if (noteCount < MAX_NOTES) {
    uint32_t finalStart = merged ? mergedStart : startTick;
    uint32_t finalEnd = merged ? mergedEnd : endTick;
    notes[noteCount++] = { pitch, velocity, finalStart, finalEnd - finalStart };
  }
  xSemaphoreGive(mutex);
}

void clearNotes() {
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    noteCount = 0;
    memset(notes, 0, sizeof(notes));
    xSemaphoreGive(mutex);
  }
}

void clearCurve(uint8_t curve) {
  if (curve >= CC_CURVE_COUNT) return;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    ccCurves[curve].pointCount = 0;
    xSemaphoreGive(mutex);
  }
}

void addOrUpdatePoint(uint8_t curve, uint32_t positionTick, uint8_t value) {
  if (curve >= CC_CURVE_COUNT) return;
  positionTick = clampTick(positionTick);
  CcCurve &target = ccCurves[curve];
  if (xSemaphoreTake(mutex, 0) != pdTRUE) return;
  for (uint16_t i = 0; i < target.pointCount; i++) {
    if (target.points[i].positionTick == positionTick) {
      target.points[i].value = value;
      xSemaphoreGive(mutex);
      return;
    }
  }
  if (target.pointCount < MAX_CC_POINTS) {
    uint16_t at = target.pointCount++;
    while (at > 0 && target.points[at - 1].positionTick > positionTick) {
      target.points[at] = target.points[at - 1];
      at--;
    }
    target.points[at] = { positionTick, value };
  }
  xSemaphoreGive(mutex);
}

uint8_t curveValueAt(const CcCurve &curve, uint32_t positionTick) {
  if (curve.pointCount == 0) return 0;
  if (curve.pointCount == 1) return curve.points[0].value;
  if (positionTick <= curve.points[0].positionTick) return curve.points[0].value;
  for (uint16_t i = 1; i < curve.pointCount; i++) {
    const CcPoint &a = curve.points[i - 1];
    const CcPoint &b = curve.points[i];
    if (positionTick <= b.positionTick) {
      uint32_t span = b.positionTick - a.positionTick;
      uint32_t offset = positionTick - a.positionTick;
      int value = a.value + ((int)b.value - a.value) * (int)offset /
                  (int)(span == 0 ? 1 : span);
      return (uint8_t)constrain(value, 0, 127);
    }
  }
  return curve.points[curve.pointCount - 1].value;
}

void init() {
  mutex = xSemaphoreCreateMutex();
  memset(notes, 0, sizeof(notes));
  memset(ccCurves, 0, sizeof(ccCurves));
  settings = Settings();
  for (uint8_t i = 0; i < CC_CURVE_COUNT; i++) {
    ccCurves[i].ccNumber = (uint8_t)(i + 1);
    ccCurves[i].muted = true;
    ccCurves[i].inputMode = CcInputMode::MIDI_IN;
    ccCurves[i].localValue = 64;
  }
  noteCount = 0;
  lastInputPitch = 60;
  activeCcCurve = 0;
  transport = TransportState::STOPPED;
  recording = RecordingState::PLAY;
  stopTick = 0;
  appMode = AppMode::NOTE_LOOP;
  updateLoopDuration();
}

} // namespace Looper