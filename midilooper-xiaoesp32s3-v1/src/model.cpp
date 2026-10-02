// =============================================================================
//  model.cpp - loop data storage, note merge/overdub, CC curve interpolation
// =============================================================================
#include "model.h"
#include <string.h>
#include <algorithm>

namespace Model {

SemaphoreHandle_t mutex = nullptr;

Note     notes[MAX_NOTES];
uint16_t noteCount = 0;

CcCurve ccCurves[CC_CURVE_COUNT];
uint8_t selectedCurve = 0;

Settings  settings;
QuantMode quantMode = QuantMode::OFF;

volatile uint8_t lastInputPitch = 60;

static uint8_t noteIdCounter = 1;

static uint8_t allocateNoteId() {
  // Skip any id currently in use (ids only wrap after 255 notes created
  // without being deleted, which cannot happen given MAX_NOTES capacity).
  for (uint16_t guard = 0; guard < 256; guard++) {
    uint8_t candidate = noteIdCounter++;
    if (findIndexById(candidate) < 0) return candidate;
  }
  return 0;
}

uint32_t loopLengthTicks() {
  return (uint32_t)settings.bars * TICKS_PER_BAR;
}

uint32_t clampToLoop(uint32_t tick) {
  uint32_t length = loopLengthTicks();
  if (length == 0) return 0;
  return tick >= length ? length - 1 : tick;
}

uint32_t quantStepTicks(QuantMode mode) {
  switch (mode) {
    case QuantMode::BEAT:    return TICKS_PER_BEAT;
    case QuantMode::HALF:    return TICKS_PER_BEAT / 2;
    case QuantMode::QUARTER: return TICKS_PER_BEAT / 4;
    case QuantMode::EIGHTH:  return TICKS_PER_BEAT / 8;
    default:                 return 0; // OFF
  }
}

uint32_t quantizeTick(uint32_t tick) {
  uint32_t step = quantStepTicks(quantMode);
  if (step == 0) return tick;
  uint32_t half = step / 2;
  return ((tick + half) / step) * step;
}

int16_t findIndexById(uint8_t id) {
  for (uint16_t i = 0; i < noteCount; i++)
    if (notes[i].id == id) return (int16_t)i;
  return -1;
}

int16_t findIndexByPitch(uint8_t pitch) {
  for (uint16_t i = 0; i < noteCount; i++)
    if (notes[i].pitch == pitch) return (int16_t)i;
  return -1;
}

uint8_t addOrMergeNote(uint8_t pitch, uint8_t velocity, uint32_t startTick, uint32_t endTick) {
  if (endTick <= startTick) endTick = startTick + 1;
  uint32_t length = loopLengthTicks();
  if (length == 0) return 255;
  startTick = clampToLoop(startTick);
  endTick = std::min(endTick, length);

  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 255;

  // Merge with any existing same-pitch note(s) that overlap in time (a shared
  // endpoint, e.g. one note's end == another's start, does not count as an
  // overlap and both notes are kept separate).
  uint32_t mergedStart = startTick;
  uint32_t mergedEnd = endTick;
  int16_t survivorId = -1;
  bool merged = false;
  for (uint16_t i = 0; i < noteCount; ) {
    bool overlaps = notes[i].pitch == pitch &&
                    startTick < notes[i].endTick() && notes[i].startTick < endTick;
    if (overlaps) {
      mergedStart = std::min(mergedStart, notes[i].startTick);
      mergedEnd = std::max(mergedEnd, notes[i].endTick());
      if (survivorId < 0 || notes[i].id < (uint8_t)survivorId) survivorId = notes[i].id;
      merged = true;
      for (uint16_t j = i; j + 1 < noteCount; j++) notes[j] = notes[j + 1];
      noteCount--;
      continue; // re-check the same index; the array just shifted
    }
    i++;
  }

  uint8_t resultId = 255;
  if (noteCount < MAX_NOTES) {
    Note note;
    note.id = merged ? (uint8_t)survivorId : allocateNoteId();
    note.pitch = pitch;
    note.velocity = velocity;
    note.startTick = mergedStart;
    note.durationTick = mergedEnd - mergedStart;
    notes[noteCount++] = note;
    resultId = note.id;
  }

  xSemaphoreGive(mutex);
  return resultId;
}

bool deleteNoteById(uint8_t id) {
  bool removed = false;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    int16_t idx = findIndexById(id);
    if (idx >= 0) {
      for (uint16_t i = (uint16_t)idx; i + 1 < noteCount; i++) notes[i] = notes[i + 1];
      noteCount--;
      removed = true;
    }
    xSemaphoreGive(mutex);
  }
  return removed;
}

void clearAllNotes() {
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    noteCount = 0;
    memset(notes, 0, sizeof(notes));
    xSemaphoreGive(mutex);
  }
}

bool getNoteCopy(uint8_t id, Note &out) {
  bool found = false;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    int16_t idx = findIndexById(id);
    if (idx >= 0) { out = notes[idx]; found = true; }
    xSemaphoreGive(mutex);
  }
  return found;
}

int16_t firstNoteId() {
  int16_t best = -1;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    for (uint16_t i = 0; i < noteCount; i++)
      if (best < 0 || notes[i].id < (uint8_t)best) best = notes[i].id;
    xSemaphoreGive(mutex);
  }
  return best;
}

int16_t lastNoteId() {
  int16_t best = -1;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    for (uint16_t i = 0; i < noteCount; i++)
      if (best < 0 || notes[i].id > (uint8_t)best) best = notes[i].id;
    xSemaphoreGive(mutex);
  }
  return best;
}

int16_t nextNoteId(uint8_t id) {
  int16_t best = -1;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    for (uint16_t i = 0; i < noteCount; i++)
      if (notes[i].id > id && (best < 0 || notes[i].id < (uint8_t)best)) best = notes[i].id;
    xSemaphoreGive(mutex);
  }
  return best < 0 ? firstNoteId() : best;
}

int16_t prevNoteId(uint8_t id) {
  int16_t best = -1;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    for (uint16_t i = 0; i < noteCount; i++)
      if (notes[i].id < id && (best < 0 || notes[i].id > (uint8_t)best)) best = notes[i].id;
    xSemaphoreGive(mutex);
  }
  return best < 0 ? lastNoteId() : best;
}

bool setNoteStart(uint8_t id, uint32_t newStart) {
  bool ok = false;
  uint32_t maxStart = (uint32_t)MAX_BARS * TICKS_PER_BAR - 1;
  if (newStart > maxStart) newStart = maxStart;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    int16_t idx = findIndexById(id);
    if (idx >= 0) { notes[idx].startTick = newStart; ok = true; }
    xSemaphoreGive(mutex);
  }
  return ok;
}

bool setNoteDuration(uint8_t id, uint32_t newDuration) {
  bool ok = false;
  if (newDuration < 1) newDuration = 1;
  uint32_t maxDur = (uint32_t)MAX_BARS * TICKS_PER_BAR;
  if (newDuration > maxDur) newDuration = maxDur;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    int16_t idx = findIndexById(id);
    if (idx >= 0) { notes[idx].durationTick = newDuration; ok = true; }
    xSemaphoreGive(mutex);
  }
  return ok;
}

bool setNoteVelocity(uint8_t id, uint8_t newVelocity) {
  bool ok = false;
  newVelocity = (uint8_t)constrain((int)newVelocity, 0, 127);
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    int16_t idx = findIndexById(id);
    if (idx >= 0) { notes[idx].velocity = newVelocity; ok = true; }
    xSemaphoreGive(mutex);
  }
  return ok;
}

void setNotePitch(uint8_t id, uint8_t newPitch) {
  newPitch = (uint8_t)constrain((int)newPitch, 0, 127);
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  int16_t idx = findIndexById(id);
  if (idx < 0) { xSemaphoreGive(mutex); return; }
  notes[idx].pitch = newPitch;
  // Merge with any other note that now shares this pitch AND overlaps this
  // note's time span: keep this note's id and velocity, widen start/end.
  for (uint16_t i = 0; i < noteCount; ) {
    bool overlaps = (int16_t)i != idx && notes[i].pitch == newPitch &&
                    notes[idx].startTick < notes[i].endTick() &&
                    notes[i].startTick < notes[idx].endTick();
    if (overlaps) {
      uint32_t mergedStart = std::min(notes[idx].startTick, notes[i].startTick);
      uint32_t mergedEnd = std::max(notes[idx].endTick(), notes[i].endTick());
      notes[idx].startTick = mergedStart;
      notes[idx].durationTick = mergedEnd - mergedStart;
      for (uint16_t j = i; j + 1 < noteCount; j++) notes[j] = notes[j + 1];
      noteCount--;
      if (idx > (int16_t)i) idx--;
      continue;
    }
    i++;
  }
  xSemaphoreGive(mutex);
}

uint8_t createNoteAt(uint32_t startTick) {
  return addOrMergeNote(lastInputPitch, 100, startTick, startTick + TICKS_PER_BEAT);
}

void addOrUpdatePoint(uint8_t curveIdx, uint32_t positionTick, uint8_t value) {
  if (curveIdx >= CC_CURVE_COUNT) return;
  positionTick = clampToLoop(positionTick);
  value = (uint8_t)constrain((int)value, 0, 127);
  CcCurve &curve = ccCurves[curveIdx];

  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  for (uint16_t i = 0; i < curve.pointCount; i++) {
    if (curve.points[i].positionTick == positionTick) {
      curve.points[i].value = value;
      xSemaphoreGive(mutex);
      return;
    }
  }
  if (curve.pointCount < MAX_CC_POINTS) {
    uint16_t at = curve.pointCount++;
    while (at > 0 && curve.points[at - 1].positionTick > positionTick) {
      curve.points[at] = curve.points[at - 1];
      at--;
    }
    curve.points[at] = { positionTick, value };
  }
  xSemaphoreGive(mutex);
}

void clearCurve(uint8_t curveIdx) {
  if (curveIdx >= CC_CURVE_COUNT) return;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    ccCurves[curveIdx].pointCount = 0;
    xSemaphoreGive(mutex);
  }
}

void erasePointsAfter(uint8_t curveIdx, uint32_t tick) {
  if (curveIdx >= CC_CURVE_COUNT) return;
  CcCurve &curve = ccCurves[curveIdx];
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  uint16_t kept = 0;
  for (uint16_t i = 0; i < curve.pointCount; i++)
    if (curve.points[i].positionTick <= tick) curve.points[kept++] = curve.points[i];
  curve.pointCount = kept;
  xSemaphoreGive(mutex);
}

uint8_t curveValueAt(uint8_t curveIdx, uint32_t positionTick) {
  if (curveIdx >= CC_CURVE_COUNT) return 0;
  const CcCurve &curve = ccCurves[curveIdx];
  if (curve.pointCount == 0) return 0;
  if (curve.pointCount == 1) return curve.points[0].value;
  if (positionTick <= curve.points[0].positionTick) return curve.points[0].value;
  for (uint16_t i = 1; i < curve.pointCount; i++) {
    const CcPoint &a = curve.points[i - 1];
    const CcPoint &b = curve.points[i];
    if (positionTick <= b.positionTick) {
      uint32_t span = b.positionTick - a.positionTick;
      uint32_t offset = positionTick - a.positionTick;
      int value = a.value + ((int)b.value - (int)a.value) * (int)offset / (int)(span == 0 ? 1 : span);
      return (uint8_t)constrain(value, 0, 127);
    }
  }
  return curve.points[curve.pointCount - 1].value;
}

void noteName(uint8_t pitch, char *buf, size_t len) {
  static const char *names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  int octave = (int)(pitch / 12) - 1;
  snprintf(buf, len, "%s%d", names[pitch % 12], octave);
}

void init() {
  if (mutex == nullptr) mutex = xSemaphoreCreateMutex();
  noteCount = 0;
  memset(notes, 0, sizeof(notes));
  noteIdCounter = 1;

  memset(ccCurves, 0, sizeof(ccCurves));
  for (uint8_t i = 0; i < CC_CURVE_COUNT; i++) {
    ccCurves[i].ccNumber = (uint8_t)(i + 1);
    // Curve 1 is unmuted and LOCAL by default; the rest start muted/MIDI IN.
    ccCurves[i].muted = (i != 0);
    ccCurves[i].inputMode = (i == 0) ? CcInputMode::LOCAL : CcInputMode::MIDI_IN;
    ccCurves[i].pointCount = 0;
  }
  selectedCurve = 0;

  settings = Settings();
  quantMode = QuantMode::OFF;
  lastInputPitch = 60;
}

} // namespace Model
