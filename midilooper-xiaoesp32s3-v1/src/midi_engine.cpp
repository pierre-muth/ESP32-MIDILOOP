// =============================================================================
//  midi_engine.cpp - timing-critical MIDI task: input capture, recording,
//                    note/CC playback scheduling (core 0, high priority)
//
//  USB MIDI on the XIAO ESP32-S3 runs in HOST mode on its single OTG port
//  (the external instrument, e.g. OP-1 field, acts as USB device - the only
//  combination that delivers multi-note messages reliably, see the host
//  diagnostics). MidiEngine owns the USBHost stack (its worker runs TUH in
//  the background) and this task pumps the mounted MIDI interface every tick.
//  Channel messages are dispatched straight from each 4-byte USB-MIDI event
//  packet (the OP-1 field sends proper status bytes; packets carrying
//  running-status data are re-materialized against the remembered status).
//  Control Surface was removed for this mode: its USBMIDI_Interface is a
//  device-stack (TUD) MIDI driver and cannot share the OTG port with host.
// =============================================================================
#include "midi_engine.h"
#include "transport.h"
#include <USBHost.h>
#include <host-overrides.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string.h>
#include <algorithm>

namespace MidiEngine {

volatile bool    activeInputNote[128];
volatile uint8_t lastCcInputValue[CC_CURVE_COUNT];
volatile uint8_t localCcValue = 64;
// volatile bool    localCcFollowing = true;

// =============================================================================
//  USB-MIDI HOST plumbing
// =============================================================================

// Single mounted interface index (OP-1 field exposes one MIDI streaming
// interface). 0xFF = nothing mounted yet.
static volatile uint8_t midIdx = 0xFF;
// Last channel-voice status byte seen from the instrument, for packets that
// only carry running-status data (CIN code-low variants).
static uint8_t rxRunStatus = 0;

// --- Connected-device status (Settings page) -------------------------------
// Written by TUH callbacks (worker task), read as a snapshot by the display.
static portMUX_TYPE                    devMux = portMUX_INITIALIZER_UNLOCKED;
static MidiEngine::DeviceInfo          devInfo = {};

extern "C" void tuh_enum_descriptor_device_cb(uint8_t daddr, const tusb_desc_device_t *desc) {
  // Captured on every attach. (tuh_mount_cb is claimed by the core's USBHost
  // wrapper, this descriptor hook is the free one for device identity.)
  portENTER_CRITICAL(&devMux);
  devInfo.vid = desc->idVendor;
  devInfo.pid = desc->idProduct;
  portEXIT_CRITICAL(&devMux);
}

extern "C" void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *m) {
  DBG_PRINTF("[midi] mounted daddr=%u rx=%u tx=%u\r\n", m->daddr,
             m->rx_cable_count, m->tx_cable_count);
  portENTER_CRITICAL(&devMux);
  devInfo.present   = true;
  devInfo.cablesIn  = m->rx_cable_count;
  devInfo.cablesOut = m->tx_cable_count;
  devInfo.mountMs   = millis();
  portEXIT_CRITICAL(&devMux);
  midIdx = idx;
  rxRunStatus = 0; // fresh device, fresh stream
}

extern "C" void tuh_midi_umount_cb(uint8_t idx) {
  DBG_PRINTF("[midi] unmounted idx=%u\r\n", idx);
  if (midIdx == idx) midIdx = 0xFF;
  rxRunStatus = 0; // the pending running status belonged to the removed device
  portENTER_CRITICAL(&devMux);
  devInfo.present = false;
  portEXIT_CRITICAL(&devMux);
}

extern "C" void tuh_midi_descriptor_cb(uint8_t, const tuh_midi_descriptor_cb_t *) {}
extern "C" void tuh_midi_rx_cb(uint8_t, uint32_t) { /* pump happens in our task */ }
extern "C" void tuh_midi_tx_cb(uint8_t, uint32_t) {}

// --- Bridge to the display/UI ---------------------------------------------
MidiEngine::DeviceInfo deviceInfo() {
  portENTER_CRITICAL(&devMux);
  MidiEngine::DeviceInfo snapshot = devInfo;
  portEXIT_CRITICAL(&devMux);
  return snapshot;
}

// =============================================================================

static uint8_t outputChannelIdx() {
  uint8_t ch = Model::settings.outputChannel;
  if (ch < 1 || ch > 16) ch = 1;
  return ch - 1;
}

// Builds a USB-MIDI event packet (CIN = status >> 4 for channel voice
// messages) and ships it to the mounted interface right away.
static bool txRaw(uint8_t status, uint8_t d1, uint8_t d2) {
  uint8_t idx = midIdx;
  if (idx == 0xFF) return false;
  const uint8_t packet[4] = { (uint8_t)(status & 0xF0), (uint8_t)(status | outputChannelIdx()),
                              (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F) };
  bool ok = tuh_midi_packet_write(idx, packet);
  if (ok) tuh_midi_write_flush(idx);
  return ok;
}

static bool txNoteOn(uint8_t pitch, uint8_t velocity)  { return txRaw(0x90, pitch, velocity); }
static bool txNoteOff(uint8_t pitch)                   { return txRaw(0x80, pitch, 0); }
static bool txControlChange(uint8_t cc, uint8_t value) { return txRaw(0xB0, cc, value); }

// =============================================================================
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

static void finalizeHeldNote(uint8_t pitch, uint32_t endOverride, bool useOverride) {
  if (!held[pitch].active) return;
  uint32_t start = held[pitch].startTick;
  uint32_t end = useOverride ? endOverride
               : (Transport::state == TransportState::RUNNING ? Transport::currentTick()
                                                                : Transport::lastStopTick);
  if (end <= start) end = start + 1;
  Model::addOrMergeNote(pitch, held[pitch].velocity, start, end);
  held[pitch].active = false;
}

static void finalizeAllHeldNotes() {
  for (uint16_t pitch = 0; pitch < 128; pitch++) finalizeHeldNote((uint8_t)pitch, 0, false);
}

// Used when a one-shot (ARMED_ONCE) note recording ends exactly at the loop
// boundary: the tick has already wrapped back near 0, so the real end
// position (the loop length) must be passed in explicitly.
static void finalizeAllHeldNotesAt(uint32_t endTick) {
  for (uint16_t pitch = 0; pitch < 128; pitch++) finalizeHeldNote((uint8_t)pitch, endTick, true);
}

static void handleNoteOn(uint8_t pitch, uint8_t velocity) {
  Model::lastInputPitch = pitch;
  activeInputNote[pitch] = true;

  if (Transport::state == TransportState::STOPPED &&
      (Transport::noteRecord == RecordState::ARMED || Transport::noteRecord == RecordState::ARMED_ONCE)) {
    Transport::start(); // auto-transitions noteRecord to RECORD
  }
  if (Transport::noteRecord == RecordState::RECORD && Transport::state == TransportState::RUNNING) {
    held[pitch].active = true;
    held[pitch].startTick = Transport::currentTick();
    held[pitch].velocity = velocity;
  }
}

static void handleNoteOff(uint8_t pitch) {
  activeInputNote[pitch] = false;
  finalizeHeldNote(pitch, 0, false);
}

static void handleCcIn(uint8_t ccNumber, uint8_t value) {
  uint8_t curveIdx = Model::selectedCurve;
  CcCurve &curve = Model::ccCurves[curveIdx];
  if (curve.ccNumber != ccNumber) return;
  lastCcInputValue[curveIdx] = value;
  if (curve.inputMode != CcInputMode::MIDI_IN) return;

  if (Transport::state == TransportState::STOPPED &&
      (Transport::ccRecord == RecordState::ARMED || Transport::ccRecord == RecordState::ARMED_ONCE)) {
    Transport::start(); // auto-transitions ccRecord to RECORD
  }
  if (Transport::ccRecord == RecordState::RECORD && Transport::state == TransportState::RUNNING) {
    Model::addOrUpdatePoint(curveIdx, Transport::currentTick(), value);
  }
}

// --- RX: USB-MIDI event packets -> channel messages ------------------------
// The OP-1 field sends proper USB-MIDI packets (CIN + full status byte), so
// messages are dispatched straight from each 4-byte event. Packets with
// CIN < 0x8 (running-status code-low variants, 0x2/0x3/0x4) are re-materialized
// against the remembered status byte. Control Surface is gone, so this parser
// replaces EngineCallbacks::onChannelMessage.

static void dispatchChannelMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  const uint8_t channel = (status & 0x0F) + 1;      // 1-based, for the MIDI monitor
  const uint8_t type    = status & 0xF0;
  bool noteOn          = type == 0x90 && d2 > 0;
  bool noteOff         = type == 0x80 || (type == 0x90 && d2 == 0);
  const uint8_t pitch  = d1 & 0x7F;
  const uint8_t value  = d2 & 0x7F;

  // Log every incoming channel message for the Settings page MIDI monitor
  // (before any routing, so unmatched messages show up, too).
  if (noteOn)          Model::logMidi(Model::MidiLogType::NOTE_ON, channel, pitch, value);
  else if (noteOff)    Model::logMidi(Model::MidiLogType::NOTE_OFF, channel, pitch, value);
  else if (type == 0xB0) Model::logMidi(Model::MidiLogType::CC, channel, pitch, value);
  else                 Model::logMidi(Model::MidiLogType::OTHER, channel, pitch, value);

  if (noteOn)       handleNoteOn(pitch, value);
  else if (noteOff) handleNoteOff(pitch);
  // Deliberately no live pass-through of MIDI input (see description-v2.md).
  else if (type == 0xB0) handleCcIn(d1 & 0x7F, value);
}

static void dispatchFromPacket(const uint8_t *p) {
  const uint8_t cin = p[0];
  if (cin < 0x8) {
    // CIN 0x1: single-byte events (real-time); 0x2-0x4: running-status data.
    if (rxRunStatus == 0 || p[1] >= 0x80) return;
    switch (cin >> 4) {
      case 2: dispatchChannelMessage(rxRunStatus, p[1], 0); break;
      case 3:
      case 4: dispatchChannelMessage(rxRunStatus, p[1], p[2]); break;
    }
    return;
  }
  const uint8_t status = p[1];
  if (status < 0x80) return;
  if ((status & 0xF0) != 0xF0) rxRunStatus = status;
  // Single-data-byte messages (PC / channel pressure) must not log whatever
  // garbage the packet carries in its last slot.
  const uint8_t d2 = (cin == 0xC0 || cin == 0xD0) ? 0 : p[3];
  dispatchChannelMessage(status, p[2], d2);
}

static uint8_t pumpLastIdx = 0xFF;
static void pumpMidiRx() {
  uint8_t idx = midIdx;
  if (idx == 0xFF) {
    if (pumpLastIdx != 0xFF) {  // just unmounted
      // Drop pending note-off state so a remounted instrument doesn't stay
      // silent for stuck notes.
      memset(outputActive, 0, sizeof(outputActive));
      pumpLastIdx = 0xFF;
    }
    return;
  }
  if (!tuh_midi_mounted(idx)) return;
  pumpLastIdx = idx;
  // NOTE: packet_read_n's bufsize and return value are BYTES (rounded down to
  // a multiple of 4), not packets - iterate nb/4 event packets only, never the
  // whole buffer (stale bytes would replay old events with fresh timestamps).
  for (;;) {
    uint8_t batch[4 * 16];
    const uint32_t nb = tuh_midi_packet_read_n(idx, batch, sizeof(batch));
    if (nb == 0) break;
    const uint32_t count = nb / 4;
    for (uint32_t i = 0; i < count; i++) dispatchFromPacket(&batch[4 * i]);
  }
}

static bool crossed(uint32_t prev, uint32_t cur, uint32_t target) {
  return target > prev && target <= cur;
}

static uint32_t effectiveStart(const Note &n, uint32_t length) {
  if (Model::quantMode == QuantMode::OFF) return n.startTick;
  return std::min(Model::quantizeTick(n.startTick), length);
}

static uint32_t effectiveEnd(const Note &n, uint32_t length) {
  uint32_t e = effectiveStart(n, length) + n.durationTick;
  return e > length ? length : e;
}

static void flushAllNoteOffs() {
  for (uint16_t pitch = 0; pitch < 128; pitch++) {
    if (outputActive[pitch]) {
      txNoteOff((uint8_t)pitch);
      outputActive[pitch] = false;
    }
  }
}

static void scheduleNotes(uint32_t tick, bool wrapped) {
  uint32_t length = Model::loopLengthTicks();

  if (wrapped) {
    flushAllNoteOffs();
    if (xSemaphoreTake(Model::mutex, 0) == pdTRUE) {
      for (uint16_t i = 0; i < Model::noteCount; i++) {
        const Note &n = Model::notes[i];
        if (n.startTick >= length) continue;
        if (effectiveStart(n, length) == 0) {
          txNoteOn(n.pitch, n.velocity);
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
    uint32_t s = effectiveStart(n, length);
    uint32_t e = effectiveEnd(n, length);
    if (e <= s) e = (s < length) ? s + 1 : length;
    if (!outputActive[n.pitch] && s > 0 && crossed(previousTick, tick, s)) {
      txNoteOn(n.pitch, n.velocity);
      outputActive[n.pitch] = true;
    }
    if (outputActive[n.pitch] && e < length && crossed(previousTick, tick, e)) {
      txNoteOff(n.pitch);
      outputActive[n.pitch] = false;
    }
  }
  xSemaphoreGive(Model::mutex);
}

static void scheduleCc(uint32_t tick, uint64_t nowUs) {
  uint8_t curveIdx = Model::selectedCurve;

  // Local "follow the curve" indicator if curve has points
  if (Transport::ccRecord == RecordState::RECORD && Transport::state == TransportState::RUNNING &&
      Model::ccCurves[curveIdx].inputMode == CcInputMode::LOCAL) {
    if (Model::curvePointCount(curveIdx) != 0) localCcValue = Model::curveValueAt(curveIdx, tick);
  }

  // Immediate output of a freshly-overridden local value, independent of rate limiting.
  if (localCcDirty) {
    localCcDirty = false;
    const CcCurve &curve = Model::ccCurves[curveIdx];
    if (!curve.muted) txControlChange(curve.ccNumber, localCcValue);
  }

  uint32_t rate = Model::settings.ccRateHz == 0 ? 1 : Model::settings.ccRateHz;
  uint32_t interval = 1000000UL / rate;
  if (nowUs < nextCcUs) return;
  nextCcUs = nowUs + interval;

  if (xSemaphoreTake(Model::mutex, 0) != pdTRUE) return;
  for (uint8_t i = 0; i < CC_CURVE_COUNT; i++) {
    const CcCurve &curve = Model::ccCurves[i];
    if (curve.muted || curve.pointCount == 0) continue;
    txControlChange(curve.ccNumber, Model::curveValueAt(i, tick));
  }
  xSemaphoreGive(Model::mutex);
}

static void servicePreview() {
  if (previewPending) {
    previewPending = false;
    if (previewActivePitch != 255) txNoteOff(previewActivePitch);
    previewActivePitch = previewPitch;
    txNoteOn(previewActivePitch, previewVelocity);
    previewOffAtUs = (uint64_t)esp_timer_get_time() + PREVIEW_DURATION_US;
  } else if (previewActivePitch != 255 && (uint64_t)esp_timer_get_time() >= previewOffAtUs) {
    txNoteOff(previewActivePitch);
    previewActivePitch = 255;
  }
}

static void taskMidi(void *) {
  memset(held, 0, sizeof(held));
  memset(outputActive, 0, sizeof(outputActive));
  memset((void *)activeInputNote, 0, sizeof(activeInputNote));

  TransportState previousTransport = TransportState::STOPPED;
  RecordState previousNoteRecord = RecordState::PLAY;

  for (;;) {
    // Pace the poll loop: back-to-back spinning at priority 10 keeps core 0
    // fully loaded, which measurably worsens rare DWC2 interrupt-latency
    // races (USB input deferred until the next message arrives). 

    taskYIELD(); // yield to other tasks (e.g. display) to avoid starving them
    vTaskDelay( pdMS_TO_TICKS(1) );

    pumpMidiRx();

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
      continue;
    }

    uint64_t nowUs = (uint64_t)esp_timer_get_time();
    uint32_t length = Model::loopLengthTicks();
    if (length == 0) { continue; }

    uint32_t tick = Transport::currentTick();
    bool justStarted = previousTransport != TransportState::RUNNING;
    bool wrapped = justStarted || tick < previousTick;

    if (wrapped && !justStarted) {
      // A recording that was auto-started from ARMED_ONCE runs for exactly
      // one cycle; notes must be finalized at the loop boundary itself,
      // since the tick has already wrapped back near 0 by this point.
      if (Transport::noteRecordAutoStop && noteRecord == RecordState::RECORD) {
        finalizeAllHeldNotesAt(length);
        Transport::noteRecord = RecordState::PLAY;
        Transport::noteRecordAutoStop = false;
        noteRecord = RecordState::PLAY;
      }
      if (Transport::ccRecordAutoStop && ccRecord == RecordState::RECORD) {
        Transport::ccRecord = RecordState::PLAY;
        Transport::ccRecordAutoStop = false;
        ccRecord = RecordState::PLAY;
      }
    }

    servicePreview();

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
  memset((void *)&rxRunStatus, 0, sizeof(rxRunStatus));

  DBG_PRINTLN("[midi] starting USB host...");
  USBHost.setCore(0); // keep TUH worker on the same core as this task
  if (!USBHost.begin()) {
    DBG_PRINTLN("[midi] USB host init FAILED - MIDI disabled this boot");
    xTaskCreatePinnedToCore(taskMidi, "midi", 8192, nullptr, 10, nullptr, 0);
    return; // pump/tx helpers see midIdx==0xFF and stay silent
  }
  DBG_PRINTLN("[midi] USB host running - connect the instrument (OTG)");

  xTaskCreatePinnedToCore(taskMidi, "midi", 32768, nullptr, 10, nullptr, 0);
}

void applyLocalCcDelta(int16_t delta) {
  if (delta == 0) return;
  uint8_t curveIdx = Model::selectedCurve;
  if (Model::ccCurves[curveIdx].inputMode != CcInputMode::LOCAL) return;

  if (Transport::state == TransportState::STOPPED &&
      (Transport::ccRecord == RecordState::ARMED || Transport::ccRecord == RecordState::ARMED_ONCE)) {

    int v = (int)localCcValue + delta * (int)Model::settings.ccKnobSensitivity;
    localCcValue = (uint8_t)constrain(v, 0, 127);
    localCcDirty = true;
  }

  if (Transport::ccRecord != RecordState::RECORD) return;

  int v = (int)localCcValue + delta * (int)Model::settings.ccKnobSensitivity;
  localCcValue = (uint8_t)constrain(v, 0, 127);
  localCcDirty = true;
  Model::addOrUpdatePoint(curveIdx, Transport::currentTick(), localCcValue);
}

void notifyCcRecordStarted() {
  uint8_t idx = Model::selectedCurve;
  if (Model::curvePointCount(idx) != 0) localCcValue = Model::curveValueAt(idx, Transport::currentTick());
  if (Transport::ccRecord == RecordState::RECORD)
    Model::addOrUpdatePoint(idx, Transport::currentTick(), localCcValue);
  
}

void requestNotePreview(uint8_t pitch, uint8_t velocity) {
  previewPitch = pitch;
  previewVelocity = velocity;
  previewPending = true;
}

} // namespace MidiEngine
