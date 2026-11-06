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
#include <host/usbh_pvt.h>
#include <host-overrides.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
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

// --- diagnostics counters (10 s heartbeat on the debug port) ----------------
// txSent: packets accepted into the TinyUSB TX FIFO (app-level)
// txFlush: packets handed to the DWC2 controller (bus submission)
// txBusy : flush calls finding the endpoint busy (cb-driven re-arm takes over)
// txCb    : device-confirmed OUT transfer completions (ground truth)
// txFail : enqueue failures or packets dropped without sending
// rxPkt/rxEvt/rxUnk: received packets / routed channel messages / rejects
static volatile uint32_t txStatSent = 0, txStatFail = 0;
static volatile int32_t  txStatQueued = 0;
static volatile uint32_t txStatFlush = 0, txStatBusy = 0, txStatCb = 0;
static volatile uint32_t rxStatPackets = 0, rxStatEvents = 0, rxStatUnknown = 0;

// --- Connected-device status (Settings page) -------------------------------
// Written by TUH callbacks (worker task), read as a snapshot by the display.
static portMUX_TYPE                    devMux = portMUX_INITIALIZER_UNLOCKED;
static MidiEngine::DeviceInfo          devInfo = {};
// display performance snapshot, same pattern
static portMUX_TYPE                    statsMux = portMUX_INITIALIZER_UNLOCKED;

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
extern "C" void tuh_midi_tx_cb(uint8_t idx, uint32_t xferred_bytes) {
  txStatCb = txStatCb + xferred_bytes / 4; // completed OUT packets
}

// --- Bridge to the display/UI ---------------------------------------------
static MidiEngine::DisplayStats gDisplayStats = {};
MidiEngine::DeviceInfo deviceInfo() {
  portENTER_CRITICAL(&devMux);
  MidiEngine::DeviceInfo snapshot = devInfo;
  portEXIT_CRITICAL(&devMux);
  return snapshot;
}

MidiEngine::DisplayStats displayStats() {
  portENTER_CRITICAL(&statsMux);
  MidiEngine::DisplayStats snapshot = gDisplayStats;
  portEXIT_CRITICAL(&statsMux);
  return snapshot;
}

// =============================================================================
//  TX - playback output to the instrument (OUT endpoint, cable 0)
//
//  TinyUSB host endpoints must only be touched from the TUH task context
//  (usbh_edpt_claim/xfer race with midih_xfer_cb re-arming otherwise, stalling
//  transfers permanently). The MIDI task therefore only *enqueues* events
//  into a byte ring; a function deferred with usbh_defer_func() drains the
//  ring and does packet_write/write_flush inside the usbhTuh worker.
// =============================================================================

// ring holds raw MIDI messages as 3 bytes: status | d1 | d2
static RingbufHandle_t txRing = nullptr;
static const size_t    TX_MSG_LEN = 3;
// one deferred pump at a time; the running pump re-queues itself if the ring
// grew while it was draining
static volatile bool   txPumpQueued = false;

// Channel index (0-based) for the wired output channel setting.
static uint8_t outputChannelIdx() {
  uint8_t ch = Model::settings.outputChannel;
  if (ch < 1 || ch > 16) ch = 1;
  return ch - 1;
}

static void requestTxPump();

// --- runs in TUH worker context -------------------------------------------
static void txPumpTask(void *) {
  txPumpQueued = false;
  size_t len = 0;
  for (;;) {
    uint8_t *msg = (uint8_t *)xRingbufferReceive(txRing, &len, 0);
    if (msg == nullptr) break;
    uint8_t idx = midIdx; // re-read per item: the device may leave mid-drain
    if (len == TX_MSG_LEN && idx != 0xFF && tuh_midi_mounted(idx)) {
      // USB-MIDI event packet, cable 0:
      //   byte0 = (cable << 4) | CIN, CIN = status>>4 for channel voice events
      //   byte1 = channel status (channel in low nibble)
      const uint8_t packet[4] = { (uint8_t)(msg[0] >> 4),          // cable 0 << 4 | CIN
                                  (uint8_t)(msg[0] | outputChannelIdx()),
                                  (uint8_t)(msg[1] & 0x7F),
                                  (uint8_t)(msg[2] & 0x7F) };
      txStatQueued = txStatQueued - 1;
      if (tuh_midi_packet_write(idx, packet)) {
        txStatSent = txStatSent + 1;
        // flush while the OUT endpoint is idle; midih_xfer_cb re-arms later
        const uint32_t flushed = tuh_midi_write_flush(idx);
        if (flushed > 0) {
          txStatFlush = txStatFlush + flushed / 4;
        } else {
          txStatBusy = txStatBusy + 1; // endpoint busy, cb-driven re-arm carries on
        }
      } else {
        txStatFail = txStatFail + 1;
      }
    } else {
      // unmounted in the meantime, or malformed item (should not happen:
      // producers push exactly 3-byte messages) - drop it
      txStatQueued = txStatQueued - 1;
      txStatFail = txStatFail + 1;
    }
    vRingbufferReturnItem(txRing, msg);
  }
  if (txStatQueued > 0) requestTxPump();
}

static void requestTxPump() {
  if (txPumpQueued) return;
  txPumpQueued = true;
  usbh_defer_func(txPumpTask, nullptr, false);
}

// --- runs in the MIDI task: only enqueue, never touch TUH ------------------
static bool txRaw(uint8_t status, uint8_t d1, uint8_t d2) {
  if (txRing == nullptr) return false;
  const uint8_t msg[TX_MSG_LEN] = { status, (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F) };
  if (xRingbufferSend(txRing, msg, TX_MSG_LEN, 0) != pdTRUE) { txStatFail = txStatFail + 1; return false; }
  txStatQueued = txStatQueued + 1;
  requestTxPump();
  return true;
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
  rxStatEvents = rxStatEvents + 1;

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
  // USB-MIDI event packet header: p[0] = (cable << 4) | CIN
  // (cable number is the HIGH nibble, code index number the LOW nibble).
  const uint8_t cable = p[0] >> 4;
  const uint8_t cin   = p[0] & 0x0F;
  if (cable != 0) { rxStatUnknown = rxStatUnknown + 1; return; } // only cable 0 comes from the OP-1
  if (cin < 0x8) {
    // Code low packets: 0x1 = single byte (real-time etc.), 0x2-0x4 carry
    // running-status data instead of a full status byte.
    if (rxRunStatus == 0 || p[1] >= 0x80) { rxStatUnknown = rxStatUnknown + 1; return; }
    switch (cin) {
      case 0x2: dispatchChannelMessage(rxRunStatus, p[1], 0); break;
      case 0x3:
      case 0x4: dispatchChannelMessage(rxRunStatus, p[1], p[2]); break;
      default: break; // CIN 0 (misc) can't be routed into the app model
    }
    return;
  }
  const uint8_t status = p[1];
  if (status < 0x80) { rxStatUnknown = rxStatUnknown + 1; return; } // invalid MIDI stream byte
  if ((status & 0xF0) != 0xF0) rxRunStatus = status;
  // Single-data-byte messages (PC / channel pressure) must not log whatever
  // the device left in the packet's last slot.
  const uint8_t d2 = (cin == 0xC || cin == 0xD) ? 0 : p[3];
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
    rxStatPackets = rxStatPackets + nb / 4;
    const uint32_t count = nb / 4;
    for (uint32_t i = 0; i < count; i++) dispatchFromPacket(&batch[4 * i]);
  }
}

// Compact 10 s heartbeat: MIDI health + model memory usage. Kept on the
// debug port permanently - it is the fastest way to see host-stack state
// (mount, tx flow, drops) without attaching a full debugger.
static void heartbeat() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 10000) return;
  last = now;
  uint16_t notes = 0, ccPoints = 0;
  if (xSemaphoreTake(Model::mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
    notes    = Model::noteCount;
    for (uint8_t i = 0; i < CC_CURVE_COUNT; i++)
      ccPoints = ccPoints + Model::ccCurves[i].pointCount;
    xSemaphoreGive(Model::mutex);
  }
  DBG_PRINTF("[stat] t=%lus rx=%lu/%lu/%lu tx=%lu/%lu/%lu f=%lu b=%lu cb=%lu "
             "notes=%u/%u ccPts=%u/%u free=%u\r\n",
             (unsigned long)(now / 1000),
             (unsigned long)rxStatEvents, (unsigned long)rxStatUnknown,
             (unsigned long)rxStatPackets,
             (unsigned long)txStatCb, (long)txStatQueued, (unsigned long)txStatFail,
             (unsigned long)txStatFlush, (unsigned long)txStatBusy,
             (unsigned long)txStatSent,
             notes, MAX_NOTES, ccPoints, CC_CURVE_COUNT * MAX_CC_POINTS,
             (unsigned int)esp_get_minimum_free_heap_size());
}

// --- performance snapshot for the display (repaint stats) ------------------

// Display updates its repaint statistics; read by the settings heartbeat.
void setDisplayStats(Fps100_t fps100, uint32_t renderUs) {
  portENTER_CRITICAL(&statsMux);
  gDisplayStats.fps100    = fps100;
  gDisplayStats.renderUs  = renderUs;
  portEXIT_CRITICAL(&statsMux);
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
    heartbeat();

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

// --- bring-up helper --------------------------------------------------------
// The DWC2 host ISR is allocated by tusb_init() on the core that *calls*
// USBHost::begin() (USBHost::setCore only pins the worker task). Slave-mode
// host re-arms the mounted device's IN endpoint every USB frame, so that ISR
// fires constantly while a device is attached. It must therefore live on
// core 0 with the MIDI task: core 1 owns the display refresh, whose SPI
// transfer becomes visibly choppy when chopped up by the host ISR.
static SemaphoreHandle_t hostBootSem  = nullptr;
static bool              hostBootOk   = false;

static void hostBootTask(void *) {
  hostBootOk = USBHost.begin();
  xSemaphoreGive(hostBootSem);
  vTaskDelete(nullptr);
}

void start() {
  memset((void *)lastCcInputValue, 0, sizeof(lastCcInputValue));
  memset((void *)&rxRunStatus, 0, sizeof(rxRunStatus));

  DBG_PRINTLN("[midi] starting USB host...");
  txRing = xRingbufferCreate(512, RINGBUF_TYPE_NOSPLIT);

  hostBootSem = xSemaphoreCreateBinary();
  if (hostBootSem != nullptr) {
    if (xTaskCreatePinnedToCore(hostBootTask, "usbbt", 4096, nullptr,
                                configMAX_PRIORITIES - 1, nullptr, 0 /* core 0 */) == pdPASS) {
      xSemaphoreTake(hostBootSem, pdMS_TO_TICKS(5000));
      vSemaphoreDelete(hostBootSem);
    }
  }
  hostBootSem = nullptr;

  if (!hostBootOk) {
    DBG_PRINTLN("[midi] USB host init FAILED - MIDI disabled this boot");
    xTaskCreatePinnedToCore(taskMidi, "midi", 8192, nullptr, 10, nullptr, 0);
    return; // pump/tx helpers see midIdx==0xFF and stay silent
  }
  DBG_PRINTLN("[midi] USB host running - connect the instrument (OTG)");

  // Byte pump feeding the deferred TX (runs in TUH worker context).
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

