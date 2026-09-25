// =============================================================================
//  transport.cpp - musical clock and transport/recording state transitions
// =============================================================================
#include "transport.h"
#include <esp_timer.h>

namespace Transport {

volatile TransportState state = TransportState::STOPPED;
volatile RecordState noteRecord = RecordState::PLAY;
volatile RecordState ccRecord = RecordState::PLAY;
volatile bool ccRecordAutoStop = false;

volatile uint64_t loopStartUs = 0;
volatile uint32_t lastStopTick = 0;

// ticks = elapsedUs * TICKS_PER_BEAT * bpm / 60e6. This ratio does not depend
// on the loop length in bars, so changing bar count never perturbs timing.
static uint32_t ticksFromUs(uint64_t us, uint16_t bpm) {
  return (uint32_t)((us * TICKS_PER_BEAT * bpm) / 60000000ULL);
}

static uint64_t usFromTicks(uint32_t ticks, uint16_t bpm) {
  if (bpm == 0) return 0;
  return ((uint64_t)ticks * 60000000ULL) / ((uint64_t)TICKS_PER_BEAT * bpm);
}

void init() {
  state = TransportState::STOPPED;
  noteRecord = RecordState::PLAY;
  ccRecord = RecordState::PLAY;
  ccRecordAutoStop = false;
  loopStartUs = 0;
  lastStopTick = 0;
}

uint32_t currentTick() {
  if (state != TransportState::RUNNING) return 0;
  uint32_t length = Model::loopLengthTicks();
  if (length == 0) return 0;
  uint64_t elapsed = (uint64_t)esp_timer_get_time() - loopStartUs;
  return ticksFromUs(elapsed, Model::settings.bpm) % length;
}

void start() {
  loopStartUs = (uint64_t)esp_timer_get_time();
  state = TransportState::RUNNING;
}

void stop() {
  lastStopTick = currentTick();
  state = TransportState::STOPPED;
  noteRecord = RecordState::PLAY;
  ccRecord = RecordState::PLAY;
  ccRecordAutoStop = false;
  loopStartUs = 0;
}

bool setBpm(int bpm) {
  if (noteRecord == RecordState::RECORD || ccRecord == RecordState::RECORD) return false;
  bpm = constrain(bpm, (int)MIN_BPM, (int)MAX_BPM);
  if (state == TransportState::RUNNING) {
    uint32_t oldTick = currentTick();
    Model::settings.bpm = (uint16_t)bpm;
    uint64_t now = (uint64_t)esp_timer_get_time();
    loopStartUs = now - usFromTicks(oldTick, (uint16_t)bpm);
  } else {
    Model::settings.bpm = (uint16_t)bpm;
  }
  return true;
}

void setBars(int bars) {
  bars = constrain(bars, (int)MIN_BARS, (int)MAX_BARS);
  Model::settings.bars = (uint8_t)bars;
  // No playhead adjustment needed: tick rate depends only on BPM, so
  // changing bar count only changes the modulo wrap point (see currentTick).
}

void toggleRunStop() {
  if (state == TransportState::STOPPED) start();
  else stop();
}

void toggleNoteRecord() {
  if (state == TransportState::STOPPED) {
    noteRecord = (noteRecord == RecordState::ARMED) ? RecordState::PLAY : RecordState::ARMED;
  } else {
    noteRecord = (noteRecord == RecordState::RECORD) ? RecordState::PLAY : RecordState::RECORD;
  }
}

void toggleCcRecord() {
  if (state == TransportState::STOPPED) {
    ccRecord = (ccRecord == RecordState::ARMED) ? RecordState::PLAY : RecordState::ARMED;
  } else {
    if (ccRecord == RecordState::RECORD) {
      ccRecord = RecordState::PLAY;
    } else {
      ccRecord = RecordState::RECORD;
      ccRecordAutoStop = false; // activated while already running: continuous
    }
  }
}

} // namespace Transport
