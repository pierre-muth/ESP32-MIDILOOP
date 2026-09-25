// =============================================================================
//  ui.cpp  -  v3 page controls and hardware-facing state changes
// =============================================================================
#include "ui.h"
#include "looper.h"
#include "midi_task.h"
#include <freertos/semphr.h>
#include <stdio.h>

namespace UI {

uint8_t focusIdx = 0;
bool editing = false;
int16_t selectedNote = -1;

static uint32_t loopTicks() {
  return (uint32_t)Looper::settings.bars * TICKS_PER_BAR;
}

void init() {
  focusIdx = 0;
  editing = false;
  selectedNote = -1;
}

uint8_t fieldCount() {
  switch (Looper::appMode) {
    case AppMode::NOTE_LOOP: return (uint8_t)NLField::_COUNT;
    case AppMode::NOTE_EDIT: return (uint8_t)NEField::_COUNT;
    case AppMode::CC_LOOP: return (uint8_t)CCField::_COUNT;
    case AppMode::SETTINGS: return (uint8_t)SField::_COUNT;
    default: return 1;
  }
}

static const char *transportText() {
  return Looper::transport == TransportState::RUNNING ? "RUN" : "STOP";
}

static const char *recordText() {
  switch (Looper::recording) {
    case RecordingState::RECORD: return "REC";
    case RecordingState::ARMED: return "ARM";
    default: return "PLAY";
  }
}

void fieldLabel(uint8_t idx, char *buf, size_t len) {
  uint8_t curve = Looper::activeCcCurve;
  switch (Looper::appMode) {
    case AppMode::NOTE_LOOP:
      switch ((NLField)idx) {
        case NLField::MODE: snprintf(buf, len, "LOOP"); break;
        case NLField::TRANSPORT: snprintf(buf, len, "%s", transportText()); break;
        case NLField::RECORDING: snprintf(buf, len, "%s", recordText()); break;
        case NLField::QUANTISE: snprintf(buf, len, "Q:%s",
              Looper::settings.quantDiv == QuantDiv::BAR ? "BAR" : "OFF"); break;
        case NLField::BPM: snprintf(buf, len, "%u BPM", Looper::settings.bpm); break;
        case NLField::BARS: snprintf(buf, len, "%u BAR", Looper::settings.bars); break;
        default: buf[0] = 0; break;
      }
      break;
    case AppMode::NOTE_EDIT:
      switch ((NEField)idx) {
        case NEField::MODE: snprintf(buf, len, "NOTE"); break;
        case NEField::NOTE_SEL: snprintf(buf, len, "ID:%d", selectedNote < 0 ? 0 : selectedNote); break;
        case NEField::NOTE_PITCH:
          snprintf(buf, len, "P:%d", selectedNote >= 0 && selectedNote < Looper::noteCount ? Looper::notes[selectedNote].pitch : 0); break;
        case NEField::NOTE_START:
          snprintf(buf, len, "S:%lu", selectedNote >= 0 && selectedNote < Looper::noteCount ? Looper::notes[selectedNote].startTick / TICKS_PER_BEAT : 0UL); break;
        case NEField::NOTE_DURATION:
          snprintf(buf, len, "D:%lu", selectedNote >= 0 && selectedNote < Looper::noteCount ? Looper::notes[selectedNote].durationTick / TICKS_PER_BEAT : 0UL); break;
        case NEField::NOTE_VEL:
          snprintf(buf, len, "V:%d", selectedNote >= 0 && selectedNote < Looper::noteCount ? Looper::notes[selectedNote].velocity : 0); break;
        case NEField::QUANTISE: snprintf(buf, len, "Q:%s", Looper::settings.quantDiv == QuantDiv::BAR ? "BAR" : "OFF"); break;
        case NEField::NEW_NOTE: snprintf(buf, len, "NEW"); break;
        case NEField::DELETE_NOTE: snprintf(buf, len, "DEL"); break;
        default: buf[0] = 0; break;
      }
      break;
    case AppMode::CC_LOOP:
      switch ((CCField)idx) {
        case CCField::MODE: snprintf(buf, len, "CC"); break;
        case CCField::TRANSPORT: snprintf(buf, len, "%s", transportText()); break;
        case CCField::CURVE: snprintf(buf, len, "CURVE:%u", curve + 1); break;
        case CCField::RECORDING: snprintf(buf, len, "%s", recordText()); break;
        case CCField::INPUT_MODE: snprintf(buf, len, "%s", Looper::ccCurves[curve].inputMode == CcInputMode::LOCAL ? "LOCAL" : "MIDI"); break;
        case CCField::BPM: snprintf(buf, len, "%u BPM", Looper::settings.bpm); break;
        case CCField::MUTE: snprintf(buf, len, "MUTE:%s", Looper::ccCurves[curve].muted ? "ON" : "OFF"); break;
        case CCField::CC_NUMBER: snprintf(buf, len, "CC:%u", Looper::ccCurves[curve].ccNumber); break;
        case CCField::CLEAR_CURVE: snprintf(buf, len, "CLEAR"); break;
        case CCField::BARS: snprintf(buf, len, "%u BAR", Looper::settings.bars); break;
        default: buf[0] = 0; break;
      }
      break;
    case AppMode::SETTINGS:
      switch ((SField)idx) {
        case SField::MODE: snprintf(buf, len, "SET"); break;
        case SField::BARS: snprintf(buf, len, "BARS:%u", Looper::settings.bars); break;
        case SField::BPM: snprintf(buf, len, "BPM:%u", Looper::settings.bpm); break;
        case SField::CC_RATE: snprintf(buf, len, "CC:%uHz", Looper::settings.ccRateHz); break;
        case SField::OUTPUT_CHANNEL: snprintf(buf, len, "CH:%u", Looper::settings.outputChannel + 1); break;
        default: buf[0] = 0; break;
      }
      break;
    default: buf[0] = 0; break;
  }
}

static void setRecording(RecordingState next) {
  if (Looper::transport == TransportState::STOPPED) {
    Looper::recording = next == RecordingState::ARMED ? RecordingState::ARMED : RecordingState::PLAY;
  } else if (next == RecordingState::RECORD || next == RecordingState::PLAY) {
    Looper::recording = next;
  }
}

static void toggleTransport() {
  if (Looper::transport == TransportState::STOPPED) {
    bool armed = Looper::recording == RecordingState::ARMED;
    Looper::startRunning();
    if (armed) Looper::recording = RecordingState::RECORD;
  } else {
    Looper::stop();
  }
}

static void toggleRecording() {
  if (Looper::transport == TransportState::STOPPED) {
    setRecording(Looper::recording == RecordingState::ARMED ? RecordingState::PLAY : RecordingState::ARMED);
  } else {
    setRecording(Looper::recording == RecordingState::RECORD ? RecordingState::PLAY : RecordingState::RECORD);
  }
}

static uint32_t editStep() {
  return Looper::settings.quantDiv == QuantDiv::BAR ? TICKS_PER_BAR : TICKS_PER_BEAT / 16;
}

static void eraseNote() {
  if (selectedNote < 0 || selectedNote >= Looper::noteCount) return;
  if (xSemaphoreTake(Looper::mutex, portMAX_DELAY) == pdTRUE) {
    for (uint16_t i = (uint16_t)selectedNote; i + 1 < Looper::noteCount; i++)
      Looper::notes[i] = Looper::notes[i + 1];
    Looper::noteCount--;
    xSemaphoreGive(Looper::mutex);
  }
  if (selectedNote >= Looper::noteCount) selectedNote = Looper::noteCount - 1;
}

static void createNote() {
  uint32_t start = selectedNote >= 0 && selectedNote < Looper::noteCount
                 ? Looper::notes[selectedNote].startTick : 0;
  Looper::addOrMergeNote(Looper::lastInputPitch, 100, start,
                         start + TICKS_PER_BAR);
  selectedNote = (int16_t)Looper::noteIndexForPitch(Looper::lastInputPitch);
}

static void applyDelta(int16_t delta) {
  uint8_t curve = Looper::activeCcCurve;
  switch (Looper::appMode) {
    case AppMode::NOTE_LOOP:
      if ((NLField)focusIdx == NLField::BPM) Looper::setBpm(Looper::settings.bpm + delta);
      else if ((NLField)focusIdx == NLField::QUANTISE && delta) Looper::settings.quantDiv =
          Looper::settings.quantDiv == QuantDiv::OFF ? QuantDiv::BAR : QuantDiv::OFF;
      break;
    case AppMode::NOTE_EDIT:
      if (selectedNote < 0 || selectedNote >= Looper::noteCount) {
        if ((NEField)focusIdx == NEField::NOTE_SEL && Looper::noteCount) selectedNote = 0;
        break;
      }
      if (xSemaphoreTake(Looper::mutex, portMAX_DELAY) != pdTRUE) break;
      if ((NEField)focusIdx == NEField::NOTE_SEL && Looper::noteCount)
        selectedNote = (selectedNote + delta + Looper::noteCount) % Looper::noteCount;
      else if ((NEField)focusIdx == NEField::NOTE_PITCH) {
        Looper::notes[selectedNote].pitch = (uint8_t)constrain((int)Looper::notes[selectedNote].pitch + delta, 0, 127);
        for (uint16_t i = 0; i < Looper::noteCount; i++) {
          if (i == selectedNote || Looper::notes[i].pitch != Looper::notes[selectedNote].pitch) continue;
          uint32_t start = min(Looper::notes[selectedNote].startTick, Looper::notes[i].startTick);
          uint32_t end = max(Looper::notes[selectedNote].startTick + Looper::notes[selectedNote].durationTick,
                             Looper::notes[i].startTick + Looper::notes[i].durationTick);
          Looper::notes[selectedNote].startTick = start;
          Looper::notes[selectedNote].durationTick = end - start;
          for (uint16_t j = i; j + 1 < Looper::noteCount; j++) Looper::notes[j] = Looper::notes[j + 1];
          Looper::noteCount--;
          if (i < selectedNote) selectedNote--;
          break;
        }
      }
      else if ((NEField)focusIdx == NEField::NOTE_START)
        Looper::notes[selectedNote].startTick = constrain((int32_t)Looper::notes[selectedNote].startTick + delta * (int32_t)editStep(), 0L, (int32_t)loopTicks() - 1);
      else if ((NEField)focusIdx == NEField::NOTE_DURATION)
        Looper::notes[selectedNote].durationTick = constrain((int32_t)Looper::notes[selectedNote].durationTick + delta * (int32_t)editStep(), 1L, (int32_t)loopTicks() - (int32_t)Looper::notes[selectedNote].startTick);
      else if ((NEField)focusIdx == NEField::NOTE_VEL)
        Looper::notes[selectedNote].velocity = (uint8_t)constrain((int)Looper::notes[selectedNote].velocity + delta, 0, 127);
      else if ((NEField)focusIdx == NEField::QUANTISE && delta)
        Looper::settings.quantDiv = Looper::settings.quantDiv == QuantDiv::OFF ? QuantDiv::BAR : QuantDiv::OFF;
      xSemaphoreGive(Looper::mutex);
      break;
    case AppMode::CC_LOOP:
      if ((CCField)focusIdx == CCField::CURVE) Looper::activeCcCurve = (curve + delta + CC_CURVE_COUNT) % CC_CURVE_COUNT;
      else if ((CCField)focusIdx == CCField::BPM) Looper::setBpm(Looper::settings.bpm + delta);
      else if ((CCField)focusIdx == CCField::INPUT_MODE &&
               Looper::ccCurves[curve].inputMode == CcInputMode::LOCAL &&
               Looper::recording != RecordingState::PLAY) {
        if (Looper::recording == RecordingState::ARMED) {
          Looper::startRunning();
          Looper::recording = RecordingState::RECORD;
        }
        Looper::ccCurves[curve].localValue = constrain(
            (int)Looper::ccCurves[curve].localValue + delta, 0, 127);
        Looper::addOrUpdatePoint(curve, Looper::currentTick(),
                                 Looper::ccCurves[curve].localValue);
        Looper::ccCurves[curve].localDirty = true;
      }
      else if ((CCField)focusIdx == CCField::INPUT_MODE && delta)
        Looper::ccCurves[curve].inputMode = Looper::ccCurves[curve].inputMode == CcInputMode::LOCAL ? CcInputMode::MIDI_IN : CcInputMode::LOCAL;
      else if ((CCField)focusIdx == CCField::MUTE && delta) Looper::ccCurves[curve].muted = !Looper::ccCurves[curve].muted;
      else if ((CCField)focusIdx == CCField::CC_NUMBER) Looper::ccCurves[curve].ccNumber = constrain((int)Looper::ccCurves[curve].ccNumber + delta, 0, 127);
      else if ((CCField)focusIdx == CCField::BARS) Looper::setBars(Looper::settings.bars + delta);
      else if ((CCField)focusIdx == CCField::RECORDING && delta) toggleRecording();
      else if ((CCField)focusIdx == CCField::CLEAR_CURVE && delta) Looper::clearCurve(curve);
      break;
    case AppMode::SETTINGS:
      if ((SField)focusIdx == SField::BARS) Looper::setBars(Looper::settings.bars + delta);
      else if ((SField)focusIdx == SField::BPM) Looper::setBpm(Looper::settings.bpm + delta);
      else if ((SField)focusIdx == SField::CC_RATE) Looper::settings.ccRateHz = constrain((int)Looper::settings.ccRateHz + delta, 1, 200);
      else if ((SField)focusIdx == SField::OUTPUT_CHANNEL) Looper::settings.outputChannel = (Looper::settings.outputChannel + delta + 16) % 16;
      break;
  }
}

static void click() {
  if (editing) { editing = false; return; }
  if (focusIdx == 0) {
    Looper::appMode = (AppMode)(((int)Looper::appMode + 1) % (int)AppMode::_COUNT);
    focusIdx = 0;
    return;
  }
  if (Looper::appMode == AppMode::NOTE_LOOP && (NLField)focusIdx == NLField::TRANSPORT) { toggleTransport(); return; }
  if (Looper::appMode == AppMode::NOTE_LOOP && (NLField)focusIdx == NLField::RECORDING) { toggleRecording(); return; }
  if (Looper::appMode == AppMode::CC_LOOP && (CCField)focusIdx == CCField::TRANSPORT) { toggleTransport(); return; }
  if (Looper::appMode == AppMode::CC_LOOP && (CCField)focusIdx == CCField::RECORDING) { toggleRecording(); return; }
  if (Looper::appMode == AppMode::NOTE_EDIT && (NEField)focusIdx == NEField::NEW_NOTE) { createNote(); return; }
  if (Looper::appMode == AppMode::NOTE_EDIT && (NEField)focusIdx == NEField::DELETE_NOTE) { eraseNote(); return; }
  if (Looper::appMode == AppMode::CC_LOOP && (CCField)focusIdx == CCField::CLEAR_CURVE) { Looper::clearCurve(Looper::activeCcCurve); return; }
  editing = true;
}

void update(int16_t encoderDelta, bool encBtnPressed, bool runStopPressed) {
  if (runStopPressed) toggleTransport();
  if (encBtnPressed) click();
  if (encoderDelta) {
    if (editing) applyDelta(encoderDelta);
    else {
      uint8_t count = fieldCount();
      focusIdx = (focusIdx + encoderDelta + count) % count;
    }
  }
}

} // namespace UI