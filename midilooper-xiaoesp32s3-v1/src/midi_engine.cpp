// =============================================================================
//  midi_engine.cpp - timing-critical MIDI task: input capture, recording,
//                     note/CC playback scheduling (core 0, high priority)
// =============================================================================
#include "midi_engine.h"
#include "transport.h"
#include <Control_Surface.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string.h>
#include <algorithm>

namespace MidiEngine {

volatile bool    activeInputNote[128];
volatile uint8_t lastCcInputValue[CC_CURVE_COUNT];
volatile uint8_t localCcValue = 64;
volatile bool    localCcFollowing = true;

static USBMIDI_Interface midi;

// --- Note recording (held input notes) --------------------------------------
struct HeldNote { bool active = false; uint32_t startTick = 0; uint8_t velocity = 0; };
static HeldNote held[128];

// --- Note preview (audition while editing notes on a stopped transport) ----
static volatile bool    previewPending = false;
static volatile uint8_t previewPitch = 0;
static volatile uint8_t previewVelocity = 100;
static uint8_t  previewActivePitch = 255;
static uint64_t previewOffAtUs = 0;
static const uint64_t PREVIEW_DURATION_US = 150000;

// --- Playback scheduling -----------------------------------------------------
static bool     outputActive[128];
static uint32_t previousTick = 0;
static uint64_t nextCcUs = 0;
static bool     localCcDirty = false;

static inline Channel outputChannel() {
  return Channel::createChannel(Model::settings.outputChannel);
}

static void finalizeHeldNote(uint8_t pitch) {
  if (!held[pitch].active) return;
  uint32_t start = held[pitch].startTick;
  uint32_t end = Transport::state == TransportState::RUNNING ? Transport::currentTick()
                                                               : Transport::lastStopTick;
  if (end <= start) end = start + 1;
  Model::addOrMergeNote(pitch, held[pitch].velocity, start, end);
  held[pitch].active = false;
}

static void finalizeAllHeldNotes() {
  for (uint16_t pitch = 0; pitch < 128; pitch++) finalizeHeldNote((uint8_t)pitch);
}

static void handleNoteOn(uint8_t pitch, uint8_t velocity) {
  Model::lastInputPitch = pitch;
  activeInputNote[pitch] = true;

  if (Transport::noteRecord == RecordState::ARMED && Transport::state == TransportState::STOPPED) {
    Transport::start();
    Transport::noteRecord = RecordState::RECORD;
  }
  if (Transport::noteRecord == RecordState::RECORD && Transport::state == TransportState::RUNNING) {
    held[pitch].active = true;
    held[pitch].startTick = Transport::currentTick();
    held[pitch].velocity = velocity;
  }
}

static void handleNoteOff(uint8_t pitch) {
  activeInputNote[pitch] = false;
  finalizeHeldNote(pitch);
}

static void handleCcIn(uint8_t ccNumber, uint8_t value) {
  uint8_t curveIdx = Model::selectedCurve;
  CcCurve &curve = Model::ccCurves[curveIdx];
  if (curve.ccNumber != ccNumber) return;
  lastCcInputValue[curveIdx] = value;
  if (curve.inputMode != CcInputMode::MIDI_IN) return;

  if (Transport::ccRecord == RecordState::ARMED && Transport::state == TransportState::STOPPED) {
    Transport::start();
    Transport::ccRecord = RecordState::RECORD;
    Transport::ccRecordAutoStop = true;
  }
  if (Transport::ccRecord == RecordState::RECORD && Transport::state == TransportState::RUNNING) {
    Model::addOrUpdatePoint(curveIdx, Transport::currentTick(), value);
  }
}

struct EngineCallbacks : MIDI_Callbacks {
  void onChannelMessage(MIDI_Interface &, ChannelMessage msg) override {
    MIDIMessageType type = msg.getMessageType();
    bool noteOnMsg = type == MIDIMessageType::NoteOn && msg.data2 > 0;
    bool noteOffMsg = type == MIDIMessageType::NoteOff ||
                      (type == MIDIMessageType::NoteOn && msg.data2 == 0);
    if (noteOnMsg || noteOffMsg) {
      uint8_t pitch = msg.data1 & 0x7F;
      if (noteOnMsg) handleNoteOn(pitch, msg.data2 & 0x7F);
      else handleNoteOff(pitch);
      return; // deliberately no live pass-through of MIDI input
    }
    if (type == MIDIMessageType::ControlChange) handleCcIn(msg.data1 & 0x7F, msg.data2 & 0x7F);
  }
};
static EngineCallbacks callbacks;

static bool crossed(uint32_t prev, uint32_t cur, uint32_t target) {
  return target > prev && target <= cur;
}

static uint32_t effectiveStart(const Note &n) {
  return Model::quantMode == QuantMode::ON ? Model::quantizeTickToBeat(n.startTick) : n.startTick;
}

static uint32_t effectiveEnd(const Note &n, uint32_t length) {
  uint32_t e = Model::quantMode == QuantMode::ON ? Model::quantizeTickToBeat(n.endTick()) : n.endTick();
  return e > length ? length : e;
}

static void flushAllNoteOffs() {
  Channel ch = outputChannel();
  for (uint16_t pitch = 0; pitch < 128; pitch++) {
    if (outputActive[pitch]) {
      midi.sendNoteOff(MIDIAddress((uint8_t)pitch, ch), 0);
      outputActive[pitch] = false;
    }
  }
}

static void scheduleNotes(uint32_t tick, bool wrapped) {
  uint32_t length = Model::loopLengthTicks();
  Channel ch = outputChannel();

  if (wrapped) {
    flushAllNoteOffs();
    if (xSemaphoreTake(Model::mutex, 0) == pdTRUE) {
      for (uint16_t i = 0; i < Model::noteCount; i++) {
        const Note &n = Model::notes[i];
        if (n.startTick >= length) continue;
        if (effectiveStart(n) == 0) {
          midi.sendNoteOn(MIDIAddress(n.pitch, ch), n.velocity);
          outputActive[n.pitch] = true;
        }
      }
      xSemaphoreGive(Model::mutex);
    }
  }

  if (xSemaphoreTake(Model::mutex, 0) != pdTRUE) return;
  for (uint16_t i = 0; i < Model::noteCount; i++) {
    const Note &n = Model::notes[i];
    if (n.startTick >= length) continue;
    uint32_t s = effectiveStart(n);
    uint32_t e = effectiveEnd(n, length);
    if (e <= s) e = (s < length) ? s + 1 : length;
    if (!outputActive[n.pitch] && s > 0 && crossed(previousTick, tick, s)) {
      midi.sendNoteOn(MIDIAddress(n.pitch, ch), n.velocity);
      outputActive[n.pitch] = true;
    }
    if (outputActive[n.pitch] && e < length && crossed(previousTick, tick, e)) {
      midi.sendNoteOff(MIDIAddress(n.pitch, ch), 0);
      outputActive[n.pitch] = false;
    }
  }
  xSemaphoreGive(Model::mutex);
}

static void scheduleCc(uint32_t tick, uint64_t nowUs) {
  Channel ch = outputChannel();
  uint8_t curveIdx = Model::selectedCurve;

  // Local "follow the curve" indicator while armed/recording but not yet
  // overridden by encoder movement.
  if (Transport::ccRecord == RecordState::RECORD && Transport::state == TransportState::RUNNING &&
      Model::ccCurves[curveIdx].inputMode == CcInputMode::LOCAL && localCcFollowing) {
    localCcValue = Model::curveValueAt(curveIdx, tick);
  }

  // Immediate output of a freshly-overridden local value, independent of rate limiting.
  if (localCcDirty) {
    localCcDirty = false;
    const CcCurve &curve = Model::ccCurves[curveIdx];
    if (!curve.muted) midi.sendControlChange(MIDIAddress(curve.ccNumber, ch), localCcValue);
  }

  uint32_t rate = Model::settings.ccRateHz == 0 ? 1 : Model::settings.ccRateHz;
  uint32_t interval = 1000000UL / rate;
  if (nowUs < nextCcUs) return;
  nextCcUs = nowUs + interval;

  if (xSemaphoreTake(Model::mutex, 0) != pdTRUE) return;
  for (uint8_t i = 0; i < CC_CURVE_COUNT; i++) {
    const CcCurve &curve = Model::ccCurves[i];
    if (curve.muted || curve.pointCount == 0) continue;
    uint8_t value = Model::curveValueAt(i, tick);
    midi.sendControlChange(MIDIAddress(curve.ccNumber, ch), value);
  }
  xSemaphoreGive(Model::mutex);
}

static void servicePreview() {
  if (previewPending) {
    previewPending = false;
    Channel ch = outputChannel();
    if (previewActivePitch != 255) midi.sendNoteOff(MIDIAddress(previewActivePitch, ch), 0);
    previewActivePitch = previewPitch;
    midi.sendNoteOn(MIDIAddress(previewActivePitch, ch), previewVelocity);
    previewOffAtUs = (uint64_t)esp_timer_get_time() + PREVIEW_DURATION_US;
  } else if (previewActivePitch != 255 && (uint64_t)esp_timer_get_time() >= previewOffAtUs) {
    midi.sendNoteOff(MIDIAddress(previewActivePitch, outputChannel()), 0);
    previewActivePitch = 255;
  }
}

static void taskMidi(void *) {
  midi.setCallbacks(callbacks);
  midi.begin();
  memset(held, 0, sizeof(held));
  memset(outputActive, 0, sizeof(outputActive));
  memset((void *)activeInputNote, 0, sizeof(activeInputNote));

  TransportState previousTransport = TransportState::STOPPED;
  RecordState previousNoteRecord = RecordState::PLAY;

  for (;;) {
    midi.update();
    servicePreview();

    TransportState transport = Transport::state;
    RecordState noteRecord = Transport::noteRecord;
    RecordState ccRecord = Transport::ccRecord;

    // Note recording left RECORD (explicit toggle or transport stop): flush held notes.
    if (previousNoteRecord == RecordState::RECORD && noteRecord != RecordState::RECORD)
      finalizeAllHeldNotes();

    if (transport == TransportState::STOPPED) {
      if (previousTransport == TransportState::RUNNING) {
        finalizeAllHeldNotes();
        flushAllNoteOffs();
      }
      previousTransport = transport;
      previousNoteRecord = noteRecord;
      previousTick = 0;
      taskYIELD();
      continue;
    }

    uint64_t nowUs = (uint64_t)esp_timer_get_time();
    uint32_t length = Model::loopLengthTicks();
    if (length == 0) { taskYIELD(); continue; }

    uint32_t tick = Transport::currentTick();
    bool justStarted = previousTransport != TransportState::RUNNING;
    bool wrapped = justStarted || tick < previousTick;

    if (wrapped && !justStarted) {
      // A CC recording that was auto-started from ARMED runs for exactly one cycle.
      if (Transport::ccRecordAutoStop && ccRecord == RecordState::RECORD) {
        Transport::ccRecord = RecordState::PLAY;
        Transport::ccRecordAutoStop = false;
        ccRecord = RecordState::PLAY;
      }
    }

    // `wrapped` also covers "just started": the playhead is at tick 0 either
    // way, so notes/CC starting at position 0 must be (re)triggered.
    scheduleNotes(tick, wrapped);
    scheduleCc(tick, nowUs);

    previousTick = tick;
    previousTransport = transport;
    previousNoteRecord = noteRecord;
  }
}

void start() {
  memset((void *)lastCcInputValue, 0, sizeof(lastCcInputValue));
  xTaskCreatePinnedToCore(taskMidi, "midi", 8192, nullptr, 10, nullptr, 0);
}

void applyLocalCcDelta(int16_t delta) {
  if (delta == 0) return;
  uint8_t curveIdx = Model::selectedCurve;
  if (Model::ccCurves[curveIdx].inputMode != CcInputMode::LOCAL) return;

  if (Transport::ccRecord == RecordState::ARMED && Transport::state == TransportState::STOPPED) {
    Transport::start();
    Transport::ccRecord = RecordState::RECORD;
    Transport::ccRecordAutoStop = true;
    localCcValue = Model::curveValueAt(curveIdx, 0);
  }
  if (Transport::ccRecord != RecordState::RECORD) return;

  localCcFollowing = false;
  int v = (int)localCcValue + delta;
  localCcValue = (uint8_t)constrain(v, 0, 127);
  localCcDirty = true;
  Model::addOrUpdatePoint(curveIdx, Transport::currentTick(), localCcValue);
}

void notifyCcRecordStarted() {
  uint8_t idx = Model::selectedCurve;
  localCcFollowing = true;
  localCcValue = Model::curveValueAt(idx, Transport::currentTick());
}

void requestNotePreview(uint8_t pitch, uint8_t velocity) {
  previewPitch = pitch;
  previewVelocity = velocity;
  previewPending = true;
}

} // namespace MidiEngine
