// =============================================================================
//  ui.cpp - per-page field tables, focus/edit navigation, blink feedback
// =============================================================================
#include "ui.h"
#include "transport.h"
#include "midi_engine.h"
#include <esp_timer.h>
#include <stdio.h>
#include <algorithm>

namespace UI {

static Page    page = Page::LOOPER;
static uint8_t focusIdx = 0;
static bool    editing = false;
static int16_t selNoteId = -1;

static int16_t  blinkIdx = -1;
static uint64_t blinkStartUs = 0;
static const uint64_t BLINK_PERIOD_US = 100000;  // one flip every 100ms
static const uint64_t BLINK_TOTAL_US  = 600000;  // ~3 flashes total

static void startBlink(uint8_t idx) {
  blinkIdx = idx;
  blinkStartUs = (uint64_t)esp_timer_get_time();
}

// Quantized edit step matches the selected quantization grid; non-quantized
// uses hundredths of a bar for fine, "analog" positioning.
static uint32_t editStep() {
  uint32_t step = Model::quantStepTicks(Model::quantMode);
  return step != 0 ? step : TICKS_PER_BAR / 100;
}

static void quantModeText(char *b, size_t n) {
  switch (Model::quantMode) {
    case QuantMode::BEAT:    snprintf(b, n, "QUANT 1"); break;
    case QuantMode::HALF:    snprintf(b, n, "QUANT 1/2"); break;
    case QuantMode::QUARTER: snprintf(b, n, "QUANT 1/4"); break;
    case QuantMode::EIGHTH:  snprintf(b, n, "QUANT 1/8"); break;
    default:                 snprintf(b, n, "QUANT OFF"); break;
  }
}
static void quantModeEncoder(int16_t delta) {
  if (!delta) return;
  int count = (int)QuantMode::COUNT;
  int v = (((int)Model::quantMode + delta) % count + count) % count;
  Model::quantMode = (QuantMode)v;
}

// ── Looper page ───────────────────────────────────────────────────────────────
static void lp_mainText(char *, size_t) {}
static void lp_runstopText(char *b, size_t n) {
  snprintf(b, n, "%s", Transport::state == TransportState::RUNNING ? "RUN" : "STOP");
}
static bool lp_runstopActive() { return Transport::state == TransportState::RUNNING; }
static void lp_recordText(char *b, size_t n) {
  switch (Transport::noteRecord) {
    case RecordState::RECORD:     snprintf(b, n, "REC"); break;
    case RecordState::ARMED:      snprintf(b, n, "ARM"); break;
    case RecordState::ARMED_ONCE: snprintf(b, n, "ARM1"); break;
    default:                      snprintf(b, n, "PLAY"); break;
  }
}
static bool lp_recordActive() { return Transport::noteRecord != RecordState::PLAY; }
static void lp_recordClick() { Transport::toggleNoteRecord(); }
static void lp_bpmText(char *b, size_t n) { snprintf(b, n, "BPM %u", Model::settings.bpm); }
static void lp_bpmEncoder(int16_t delta) { Transport::setBpm((int)Model::settings.bpm + delta); }
static void lp_barsText(char *b, size_t n) { snprintf(b, n, "%u BARS", Model::settings.bars); }
static void lp_clearText(char *b, size_t n) { snprintf(b, n, "CLEAR"); }
static void lp_clearClick() { Model::clearAllNotes(); }

static const Field looperFields[] = {
  { FieldType::MAIN,   lp_mainText,    nullptr,          nullptr,        nullptr,          false },
  { FieldType::LABEL,  lp_runstopText, nullptr,          nullptr,        lp_runstopActive, false },
  { FieldType::LABEL,  lp_recordText,  nullptr,          lp_recordClick, lp_recordActive,  false },
  { FieldType::OPTION, quantModeText,  quantModeEncoder, nullptr,        nullptr,          false },
  { FieldType::VALUE,  lp_bpmText,     lp_bpmEncoder,    nullptr,        nullptr,          false },
  { FieldType::LABEL,  lp_barsText,    nullptr,          nullptr,        nullptr,          false },
  { FieldType::LABEL,  lp_clearText,   nullptr,          lp_clearClick,  nullptr,          true },
};

// ── Note Edit page ────────────────────────────────────────────────────────────
static void ne_mainText(char *, size_t) {}

static void ne_idText(char *b, size_t n) {
  if (selNoteId < 0) snprintf(b, n, "ID --");
  else snprintf(b, n, "ID %d", selNoteId);
}
static void ne_idEncoder(int16_t delta) {
  if (delta == 0) return;
  if (selNoteId < 0) { selNoteId = Model::firstNoteId(); return; }
  selNoteId = delta > 0 ? Model::nextNoteId((uint8_t)selNoteId) : Model::prevNoteId((uint8_t)selNoteId);
}

static void ne_pitchText(char *b, size_t n) {
  Note note;
  if (selNoteId >= 0 && Model::getNoteCopy((uint8_t)selNoteId, note)) {
    char nm[8];
    Model::noteName(note.pitch, nm, sizeof(nm));
    snprintf(b, n, "%s (%d)", nm, note.pitch);
  } else snprintf(b, n, "--");
}
static void ne_pitchEncoder(int16_t delta) {
  if (selNoteId < 0) return;
  Note note;
  if (!Model::getNoteCopy((uint8_t)selNoteId, note)) return;
  int newPitch = constrain((int)note.pitch + delta, 0, 127);
  Model::setNotePitch((uint8_t)selNoteId, (uint8_t)newPitch);
  if (Transport::state == TransportState::STOPPED)
    MidiEngine::requestNotePreview((uint8_t)newPitch, note.velocity);
}

static void ne_startText(char *b, size_t n) {
  Note note;
  if (selNoteId >= 0 && Model::getNoteCopy((uint8_t)selNoteId, note))
    snprintf(b, n, "START %.2f", (double)note.startTick / TICKS_PER_BAR);
  else snprintf(b, n, "START --");
}
static void ne_startEncoder(int16_t delta) {
  if (selNoteId < 0) return;
  Note note;
  if (!Model::getNoteCopy((uint8_t)selNoteId, note)) return;
  int64_t newStart = (int64_t)note.startTick + (int64_t)delta * editStep();
  if (newStart < 0) newStart = 0;
  Model::setNoteStart((uint8_t)selNoteId, (uint32_t)newStart);
}

static void ne_durationText(char *b, size_t n) {
  Note note;
  if (selNoteId >= 0 && Model::getNoteCopy((uint8_t)selNoteId, note))
    snprintf(b, n, "DUR %.2f", (double)note.durationTick / TICKS_PER_BAR);
  else snprintf(b, n, "DUR --");
}
static void ne_durationEncoder(int16_t delta) {
  if (selNoteId < 0) return;
  Note note;
  if (!Model::getNoteCopy((uint8_t)selNoteId, note)) return;
  int64_t newDur = (int64_t)note.durationTick + (int64_t)delta * editStep();
  if (newDur < 1) newDur = 1;
  Model::setNoteDuration((uint8_t)selNoteId, (uint32_t)newDur);
  if (Transport::state == TransportState::STOPPED)
    MidiEngine::requestNotePreview(note.pitch, note.velocity);
}

static void ne_velocityText(char *b, size_t n) {
  Note note;
  if (selNoteId >= 0 && Model::getNoteCopy((uint8_t)selNoteId, note)) snprintf(b, n, "VEL %d", note.velocity);
  else snprintf(b, n, "VEL --");
}
static void ne_velocityEncoder(int16_t delta) {
  if (selNoteId < 0) return;
  Note note;
  if (!Model::getNoteCopy((uint8_t)selNoteId, note)) return;
  int newVel = constrain((int)note.velocity + delta, 0, 127);
  Model::setNoteVelocity((uint8_t)selNoteId, (uint8_t)newVel);
  if (Transport::state == TransportState::STOPPED)
    MidiEngine::requestNotePreview(note.pitch, (uint8_t)newVel);
}

static void ne_newText(char *b, size_t n) { snprintf(b, n, "NEW"); }
static void ne_newClick() {
  uint32_t start = 0;
  Note prev;
  if (selNoteId >= 0 && Model::getNoteCopy((uint8_t)selNoteId, prev)) start = prev.endTick();
  uint8_t id = Model::createNoteAt(start);
  if (id != 255) selNoteId = id;
}

static void ne_deleteText(char *b, size_t n) { snprintf(b, n, "DELETE"); }
static void ne_deleteClick() {
  if (selNoteId < 0) return;
  uint8_t deleting = (uint8_t)selNoteId;
  int16_t candidate = Model::prevNoteId(deleting);
  bool hasStrictPrev = candidate >= 0 && (uint8_t)candidate < deleting;
  Model::deleteNoteById(deleting);
  selNoteId = hasStrictPrev ? candidate : Model::firstNoteId();
}

static const Field noteEditFields[] = {
  { FieldType::MAIN,   ne_mainText,     nullptr,             nullptr,        nullptr, false },
  { FieldType::VALUE,  ne_idText,       ne_idEncoder,        nullptr,        nullptr, false },
  { FieldType::OPTION, ne_pitchText,    ne_pitchEncoder,     nullptr,        nullptr, false },
  { FieldType::VALUE,  ne_startText,    ne_startEncoder,     nullptr,        nullptr, false },
  { FieldType::VALUE,  ne_durationText, ne_durationEncoder,  nullptr,        nullptr, false },
  { FieldType::VALUE,  ne_velocityText, ne_velocityEncoder,  nullptr,        nullptr, false },
  { FieldType::OPTION, quantModeText,   quantModeEncoder,    nullptr,        nullptr, false },
  { FieldType::LABEL,  ne_newText,      nullptr,             ne_newClick,    nullptr, true },
  { FieldType::LABEL,  ne_deleteText,   nullptr,             ne_deleteClick, nullptr, true },
};

// ── CC Edit page ──────────────────────────────────────────────────────────────
static void cc_mainText(char *, size_t) {}
static void cc_runstopText(char *b, size_t n) {
  snprintf(b, n, "%s", Transport::state == TransportState::RUNNING ? "RUN" : "STOP");
}
static bool cc_runstopActive() { return Transport::state == TransportState::RUNNING; }
static void cc_curveText(char *b, size_t n) { snprintf(b, n, "CURVE %d", Model::selectedCurve + 1); }
static void cc_curveEncoder(int16_t delta) {
  if (!delta) return;
  int idx = ((int)Model::selectedCurve + delta) % CC_CURVE_COUNT;
  if (idx < 0) idx += CC_CURVE_COUNT;
  Model::selectedCurve = (uint8_t)idx;
}
static void cc_recordText(char *b, size_t n) {
  switch (Transport::ccRecord) {
    case RecordState::RECORD:     snprintf(b, n, "REC"); break;
    case RecordState::ARMED:      snprintf(b, n, "ARM"); break;
    case RecordState::ARMED_ONCE: snprintf(b, n, "ARM1"); break;
    default:                      snprintf(b, n, "PLAY"); break;
  }
}
static bool cc_recordActive() { return Transport::ccRecord != RecordState::PLAY; }
// Leaving PLAY (to ARMED/ARMED_ONCE/RECORD) (re)starts the LOCAL "follow the
// curve" baseline, so a stale state from an earlier session never skips the
// erase-ahead-of-playhead step on the next recording's first knob turn.
static void cycleCcRecord() {
  Transport::toggleCcRecord();
  if (Transport::ccRecord != RecordState::PLAY) MidiEngine::notifyCcRecordStarted();
}
static void cc_recordClick() { cycleCcRecord(); }
static void cc_inputModeText(char *b, size_t n) {
  snprintf(b, n, "%s", Model::ccCurves[Model::selectedCurve].inputMode == CcInputMode::LOCAL ? "LOCAL" : "MIDI IN");
}
static void cc_inputModeEncoder(int16_t delta) {
  if (!delta) return;
  CcCurve &c = Model::ccCurves[Model::selectedCurve];
  c.inputMode = c.inputMode == CcInputMode::LOCAL ? CcInputMode::MIDI_IN : CcInputMode::LOCAL;
}
static void cc_bpmText(char *b, size_t n) { snprintf(b, n, "BPM %u", Model::settings.bpm); }
static void cc_bpmEncoder(int16_t delta) { Transport::setBpm((int)Model::settings.bpm + delta); }
static void cc_muteText(char *b, size_t n) {
  snprintf(b, n, "MUTE %s", Model::ccCurves[Model::selectedCurve].muted ? "ON" : "OFF");
}
static void cc_muteEncoder(int16_t delta) {
  if (delta) Model::ccCurves[Model::selectedCurve].muted = !Model::ccCurves[Model::selectedCurve].muted;
}
static void cc_numberText(char *b, size_t n) { snprintf(b, n, "CC %u", Model::ccCurves[Model::selectedCurve].ccNumber); }
static void cc_numberEncoder(int16_t delta) {
  CcCurve &c = Model::ccCurves[Model::selectedCurve];
  int v = constrain((int)c.ccNumber + delta, 0, 127);
  c.ccNumber = (uint8_t)v;
}
static void cc_clearText(char *b, size_t n) { snprintf(b, n, "CLEAR"); }
static void cc_clearClick() { Model::clearCurve(Model::selectedCurve); }
static void cc_barsText(char *b, size_t n) { snprintf(b, n, "%u BARS", Model::settings.bars); }

static const Field ccEditFields[] = {
  { FieldType::MAIN,   cc_mainText,      nullptr,              nullptr,        nullptr,          false },
  { FieldType::LABEL,  cc_runstopText,   nullptr,              nullptr,        cc_runstopActive, false },
  { FieldType::OPTION, cc_curveText,     cc_curveEncoder,      nullptr,        nullptr,          false },
  { FieldType::LABEL,  cc_recordText,    nullptr,              cc_recordClick, cc_recordActive,  false },
  { FieldType::OPTION, cc_inputModeText, cc_inputModeEncoder,  nullptr,        nullptr,          false },
  { FieldType::VALUE,  cc_bpmText,       cc_bpmEncoder,        nullptr,        nullptr,          false },
  { FieldType::OPTION, cc_muteText,      cc_muteEncoder,       nullptr,        nullptr,          false },
  { FieldType::VALUE,  cc_numberText,    cc_numberEncoder,     nullptr,        nullptr,          false },
  { FieldType::LABEL,  cc_clearText,     nullptr,              cc_clearClick,  nullptr,          true },
  // { FieldType::LABEL,  cc_barsText,      nullptr,              nullptr,        nullptr,          false },
};

// ── Settings page ─────────────────────────────────────────────────────────────
static void st_mainText(char *, size_t) {}
static void st_barsText(char *b, size_t n) { snprintf(b, n, "BARS %u", Model::settings.bars); }
static void st_barsEncoder(int16_t delta) { Transport::setBars((int)Model::settings.bars + delta); }
static void st_bpmText(char *b, size_t n) { snprintf(b, n, "BPM %u", Model::settings.bpm); }
static void st_bpmEncoder(int16_t delta) { Transport::setBpm((int)Model::settings.bpm + delta); }
static void st_ccRateText(char *b, size_t n) { snprintf(b, n, "CC RATE %u Hz", Model::settings.ccRateHz); }
static void st_ccRateEncoder(int16_t delta) {
  int v = constrain((int)Model::settings.ccRateHz + delta, (int)MIN_CC_RATE_HZ, (int)MAX_CC_RATE_HZ);
  Model::settings.ccRateHz = (uint8_t)v;
}
static void st_channelText(char *b, size_t n) { snprintf(b, n, "MIDI CH %u", Model::settings.outputChannel); }
static void st_channelEncoder(int16_t delta) {
  int v = constrain((int)Model::settings.outputChannel + delta, (int)MIN_MIDI_CHANNEL, (int)MAX_MIDI_CHANNEL);
  Model::settings.outputChannel = (uint8_t)v;
}
static void st_ccSensText(char *b, size_t n) { snprintf(b, n, "CC KNOB SENS %u", Model::settings.ccKnobSensitivity); }
static void st_ccSensEncoder(int16_t delta) {
  int v = constrain((int)Model::settings.ccKnobSensitivity + delta, (int)MIN_CC_KNOB_SENS, (int)MAX_CC_KNOB_SENS);
  Model::settings.ccKnobSensitivity = (uint8_t)v;
}

static const Field settingsFields[] = {
  { FieldType::MAIN,  st_mainText,    nullptr,           nullptr, nullptr, false },
  { FieldType::VALUE, st_barsText,    st_barsEncoder,    nullptr, nullptr, false },
  { FieldType::VALUE, st_bpmText,     st_bpmEncoder,     nullptr, nullptr, false },
  { FieldType::VALUE, st_ccRateText,  st_ccRateEncoder,  nullptr, nullptr, false },
  { FieldType::VALUE, st_channelText, st_channelEncoder, nullptr, nullptr, false },
  { FieldType::VALUE, st_ccSensText,  st_ccSensEncoder,  nullptr, nullptr, false },
};

// ── Page/field lookup + navigation ────────────────────────────────────────────
static uint8_t countForPage(Page p) {
  switch (p) {
    case Page::LOOPER:    return sizeof(looperFields) / sizeof(Field);
    case Page::NOTE_EDIT: return sizeof(noteEditFields) / sizeof(Field);
    case Page::CC_EDIT:   return sizeof(ccEditFields) / sizeof(Field);
    case Page::SETTINGS:  return sizeof(settingsFields) / sizeof(Field);
    default: return 1;
  }
}
static const Field *fieldsForPage(Page p) {
  switch (p) {
    case Page::LOOPER:    return looperFields;
    case Page::NOTE_EDIT: return noteEditFields;
    case Page::CC_EDIT:   return ccEditFields;
    case Page::SETTINGS:  return settingsFields;
    default: return looperFields;
  }
}

static void switchPage() {
  page = (Page)(((uint8_t)page + 1) % (uint8_t)Page::COUNT);
  focusIdx = 0;
  editing = false;
}

static bool ccLocalRecordOverride() {
  return page == Page::CC_EDIT && Transport::ccRecord != RecordState::PLAY &&
         Model::ccCurves[Model::selectedCurve].inputMode == CcInputMode::LOCAL;
}

void init() {
  page = Page::LOOPER;
  focusIdx = 0;
  editing = false;
  selNoteId = -1;
  blinkIdx = -1;
}

void update(int16_t encoderDelta, bool encClicked) {
  // While arming or recording a LOCAL CC curve, rotation is dedicated to
  // curve value input (so the first turn itself can trigger ARMED -> RECORD).
  // Clicking still cycles ARMED -> ARMED_ONCE -> PLAY normally; it only
  // cancels straight back to Play once actually recording.
  if (ccLocalRecordOverride()) {
    if (encoderDelta) MidiEngine::applyLocalCcDelta(encoderDelta);
    if (encClicked) {
      if (Transport::ccRecord == RecordState::RECORD) Transport::exitCcRecord();
      else cycleCcRecord();
    }
    return;
  }

  if (encClicked) {
    if (focusIdx == 0) {
      switchPage();
    } else if (editing) {
      editing = false;
    } else {
      const Field &f = fieldsForPage(page)[focusIdx];
      if (f.type == FieldType::VALUE || f.type == FieldType::OPTION) {
        editing = true;
      } else if (f.type == FieldType::LABEL && f.onClick) {
        f.onClick();
        if (f.momentary) startBlink(focusIdx);
      }
    }
  }

  if (encoderDelta) {
    uint8_t count = countForPage(page);
    if (editing) {
      const Field &f = fieldsForPage(page)[focusIdx];
      if (f.onEncoder) f.onEncoder(encoderDelta);
    } else {
      int next = ((int)focusIdx + encoderDelta) % (int)count;
      if (next < 0) next += count;
      focusIdx = (uint8_t)next;
    }
  }
}

Page currentPage() { return page; }
uint8_t fieldCount() { return countForPage(page); }
const Field &fieldAt(uint8_t idx) { return fieldsForPage(page)[idx]; }
uint8_t focusIndex() { return focusIdx; }
bool isEditing() { return editing; }
int16_t selectedNoteId() { return selNoteId; }

bool isBlinking(uint8_t idx) {
  if ((int16_t)idx != blinkIdx) return false;
  uint64_t elapsed = (uint64_t)esp_timer_get_time() - blinkStartUs;
  if (elapsed >= BLINK_TOTAL_US) { blinkIdx = -1; return false; }
  return (elapsed / BLINK_PERIOD_US) % 2 == 0;
}

} // namespace UI
