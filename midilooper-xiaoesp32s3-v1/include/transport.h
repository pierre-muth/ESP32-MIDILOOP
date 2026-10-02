// =============================================================================
//  transport.h - transport & recording state machine, musical clock
// =============================================================================
#pragma once
#include <Arduino.h>
#include "model.h"

enum class TransportState : uint8_t { STOPPED, RUNNING };
// PLAY: not recording. ARMED: waiting for the first input, then records
// indefinitely. ARMED_ONCE: waiting for the first input, then records for
// exactly one loop cycle before automatically returning to PLAY.
enum class RecordState : uint8_t { PLAY, RECORD, ARMED, ARMED_ONCE };

namespace Transport {

  extern volatile TransportState state;

  // Independent recording flags: the Looper page's Play/Record button drives
  // noteRecord, the CC Edit page's Play/Record button drives ccRecord. They
  // share the same transport/playhead but record different data (see
  // description-v2.md Looper/CC Edit sections).
  extern volatile RecordState noteRecord;
  extern volatile RecordState ccRecord;

  // True while noteRecord/ccRecord is running only because it was armed via
  // ARMED_ONCE; such a recording session auto-reverts to PLAY after one full
  // loop cycle (see MidiEngine's loop-wrap handling).
  extern volatile bool noteRecordAutoStop;
  extern volatile bool ccRecordAutoStop;

  extern volatile uint64_t loopStartUs;
  // Tick captured at the instant the transport last stopped; used to finalize
  // notes/CC that were being recorded when the stop occurred.
  extern volatile uint32_t lastStopTick;

  void init();

  // Current playhead position in ticks (0 when stopped).
  uint32_t currentTick();

  // Starts the playhead running from position 0. If noteRecord/ccRecord is
  // ARMED or ARMED_ONCE, immediately converts it to an actual RECORD session
  // (this is the "loop is started" trigger, in addition to the first
  // recordable MIDI input handled by the MIDI engine).
  void start();
  // Stops the playhead, resets it to 0, and returns both recording flags to
  // PLAY. Note-off flushing is performed by the MIDI engine, which detects
  // the RUNNING -> STOPPED edge.
  void stop();

  // Returns false (and leaves bpm unchanged) if a recording is in progress.
  bool setBpm(int bpm);
  void setBars(int bars);

  // UI-facing toggles ---------------------------------------------------------
  void toggleRunStop();   // also invoked by the physical D4 button
  // While stopped, cycles PLAY -> ARMED -> ARMED_ONCE -> PLAY. While running,
  // toggles PLAY <-> RECORD directly (continuous, no auto-stop).
  void toggleNoteRecord();
  void toggleCcRecord();
  // Forces ccRecord back to PLAY unconditionally; used when the UI exits a
  // LOCAL CC recording session that has taken over the rotary encoder.
  void exitCcRecord();

} // namespace Transport
