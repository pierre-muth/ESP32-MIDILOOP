// =============================================================================
//  midi_task.cpp  -  timing-critical MIDI input, recording and playback
// =============================================================================
#include "midi_task.h"
#include "looper.h"
#include <Control_Surface.h>
#include <freertos/task.h>
#include <string.h>

USBMIDI_Interface midi;

static bool held[128];
static uint32_t heldStart[128];
static uint8_t heldVelocity[128];
static bool outputActive[128];
static uint32_t previousTick = 0;
static uint64_t previousLoop = 0;
static uint64_t nextCcUs = 0;

static void sendNoteOffs() {
  for (uint16_t pitch = 0; pitch < 128; pitch++) {
    if (outputActive[pitch]) {
      midi.sendNoteOff(MIDIAddress(pitch, Channel(Looper::settings.outputChannel)), 0);
      outputActive[pitch] = false;
    }
  }
}

static bool crossed(uint32_t previous, uint32_t current, uint32_t target) {
  return target > previous && target <= current;
}

static void beginRecordingNote(uint8_t pitch, uint8_t velocity) {
  if (Looper::appMode != AppMode::NOTE_LOOP) return;
  if (Looper::recording == RecordingState::ARMED &&
      Looper::transport == TransportState::STOPPED) {
    Looper::startRunning();
    Looper::recording = RecordingState::RECORD;
  }
  if (Looper::recording != RecordingState::RECORD ||
      Looper::transport != TransportState::RUNNING) return;
  held[pitch] = true;
  heldStart[pitch] = Looper::currentTick();
  heldVelocity[pitch] = velocity;
  Looper::lastInputPitch = pitch;
}

static void finishRecordingNote(uint8_t pitch, bool atBoundary = false) {
  if (!held[pitch]) return;
  uint32_t end = atBoundary ? (uint32_t)Looper::settings.bars * TICKS_PER_BAR
                            : Looper::currentTick();
  if (Looper::transport == TransportState::STOPPED) end = Looper::stopTick;
  if (end <= heldStart[pitch]) end = heldStart[pitch] + 1;
  Looper::addOrMergeNote(pitch, heldVelocity[pitch], heldStart[pitch], end);
  held[pitch] = false;
}

struct LooperCallbacks : MIDI_Callbacks {
  void onChannelMessage(MIDI_Interface &, ChannelMessage msg) override {
    MIDIMessageType type = msg.getMessageType();
    bool noteOn = type == MIDIMessageType::NoteOn && msg.data2 > 0;
    bool noteOff = type == MIDIMessageType::NoteOff ||
                   (type == MIDIMessageType::NoteOn && msg.data2 == 0);

    if (noteOn || noteOff) {
      uint8_t pitch = msg.data1 & 0x7F;
      if (noteOn) beginRecordingNote(pitch, msg.data2);
      else finishRecordingNote(pitch);
      return; // v3 deliberately does not live-echo MIDI input
    }

    if (type != MIDIMessageType::ControlChange) return;
    uint8_t curveIndex = Looper::activeCcCurve;
    CcCurve &curve = Looper::ccCurves[curveIndex];
    if (Looper::appMode != AppMode::CC_LOOP ||
        curve.inputMode != CcInputMode::MIDI_IN ||
        msg.data1 != curve.ccNumber) return;

    curve.lastInputValue = msg.data2 & 0x7F;
    if (Looper::recording == RecordingState::ARMED &&
        Looper::transport == TransportState::STOPPED) {
      Looper::startRunning();
      Looper::recording = RecordingState::RECORD;
    }
    if (Looper::recording == RecordingState::RECORD &&
        Looper::transport == TransportState::RUNNING)
      Looper::addOrUpdatePoint(curveIndex, Looper::currentTick(), curve.lastInputValue);
  }
} callbacks;

static void scheduleNotes(uint32_t currentTick, bool wrapped) {
  if (wrapped) {
    sendNoteOffs();
    previousTick = 0;
    for (uint16_t i = 0; i < Looper::noteCount; i++) {
      const Note &note = Looper::notes[i];
      if (note.startTick == 0) {
        MIDIAddress address(note.pitch, Channel(Looper::settings.outputChannel));
        midi.sendNoteOn(address, note.velocity);
        outputActive[note.pitch] = true;
      }
    }
  }

  if (xSemaphoreTake(Looper::mutex, 0) != pdTRUE) return;
  for (uint16_t i = 0; i < Looper::noteCount; i++) {
    const Note &note = Looper::notes[i];
    uint32_t end = note.startTick + note.durationTick;
    if (!outputActive[note.pitch] && note.startTick > 0 &&
        crossed(previousTick, currentTick, note.startTick)) {
      midi.sendNoteOn(MIDIAddress(note.pitch, Channel(Looper::settings.outputChannel)), note.velocity);
      outputActive[note.pitch] = true;
    }
    if (outputActive[note.pitch] && end < (uint32_t)Looper::settings.bars * TICKS_PER_BAR &&
        crossed(previousTick, currentTick, end)) {
      midi.sendNoteOff(MIDIAddress(note.pitch, Channel(Looper::settings.outputChannel)), 0);
      outputActive[note.pitch] = false;
    }
  }
  xSemaphoreGive(Looper::mutex);
}

static void scheduleCc(uint32_t tick, uint64_t nowUs) {
  uint32_t interval = 1000000UL / (Looper::settings.ccRateHz == 0 ? 1 : Looper::settings.ccRateHz);
  if (nowUs < nextCcUs) return;
  nextCcUs = nowUs + interval;
  if (xSemaphoreTake(Looper::mutex, 0) != pdTRUE) return;
  for (uint8_t i = 0; i < CC_CURVE_COUNT; i++) {
    CcCurve &curve = Looper::ccCurves[i];
    if (curve.localDirty) {
      if (!curve.muted)
        midi.sendControlChange(MIDIAddress(curve.ccNumber, Channel(Looper::settings.outputChannel)), curve.localValue);
      curve.localDirty = false;
    }
    if (curve.muted || curve.pointCount == 0) continue;
    uint8_t value = Looper::curveValueAt(curve, tick);
    midi.sendControlChange(MIDIAddress(curve.ccNumber, Channel(Looper::settings.outputChannel)), value);
  }
  xSemaphoreGive(Looper::mutex);
}

static void taskMidi(void *) {
  midi.setCallbacks(callbacks);
  midi.begin();
  memset(held, 0, sizeof(held));
  memset(outputActive, 0, sizeof(outputActive));

  TransportState previousTransport = TransportState::STOPPED;
  for (;;) {
    midi.update();
    TransportState transport = Looper::transport;

    if (transport == TransportState::RUNNING && previousTransport != TransportState::RUNNING) {
      previousLoop = UINT64_MAX;
      previousTick = 0;
      nextCcUs = 0;
    }

    if (transport == TransportState::STOPPED) {
      if (previousTransport == TransportState::RUNNING) {
        for (uint16_t pitch = 0; pitch < 128; pitch++) finishRecordingNote(pitch, true);
        sendNoteOffs();
      }
      previousTransport = transport;
      taskYIELD();
      continue;
    }

    uint64_t nowUs = esp_timer_get_time();
    uint32_t duration = Looper::loopDurationUs;
    if (duration == 0) { taskYIELD(); continue; }
    uint64_t elapsed = nowUs - Looper::loopStartUs;
    uint64_t loopNumber = elapsed / duration;
    uint32_t tick = Looper::tickAtUs(elapsed);
    bool wrapped = loopNumber != previousLoop;
    if (wrapped) {
      previousLoop = loopNumber;
      if (Looper::recording == RecordingState::RECORD && loopNumber > 0) {
        for (uint16_t pitch = 0; pitch < 128; pitch++) finishRecordingNote(pitch, true);
        Looper::recording = RecordingState::PLAY;
      }
    }
    scheduleNotes(tick, wrapped);
    scheduleCc(tick, nowUs);
    previousTick = tick;
    previousTransport = transport;
  }
}

void midiTaskStart() {
  xTaskCreatePinnedToCore(taskMidi, "midi", 8192, nullptr, 10, nullptr, 0);
}

void midiSendLocalCC(uint8_t curveIndex, uint8_t value) {
  if (curveIndex >= CC_CURVE_COUNT) return;
  CcCurve &curve = Looper::ccCurves[curveIndex];
  curve.lastInputValue = value;
  curve.localValue = value;
  curve.localDirty = true;
}