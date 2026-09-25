// =============================================================================
//  main.cpp - hardware bring-up, task launch, UI polling loop
// =============================================================================
#include <Arduino.h>
#include "config.h"
#include "model.h"
#include "transport.h"
#include "midi_engine.h"
#include "ui.h"
#include "display.h"
#include "input.h"

void setup() {
  DBG.begin(DBG_BAUD, SERIAL_8N1, PIN_DEBUG_RX, PIN_DEBUG_TX);
  DBG_PRINTLN("\n[setup] MIDI looper booting...");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Input::init();

  Model::init();
  Transport::init();
  UI::init();

  // The MIDI task runs a tight, non-blocking loop on core 0 for deterministic
  // timing; its idle watchdog is disabled since it deliberately never blocks
  // for long (see description-v2.md: "manage the TWDT ... accordingly").
  disableCore0WDT();
  DBG_PRINTLN("[setup] core 0 idle watchdog disabled for the MIDI task");

  MidiEngine::start();
  displayTaskStart();

  DBG_PRINTLN("[setup] done");
}

void loop() {
  if (Input::runStopButtonPressed()) Transport::toggleRunStop();

  int16_t delta = Input::consumeEncoderDelta();
  bool clicked = Input::encoderButtonPressed();
  UI::update(delta, clicked);

  bool recording = Transport::noteRecord == RecordState::RECORD ||
                    Transport::ccRecord == RecordState::RECORD;
  digitalWrite(LED_BUILTIN, recording ? LOW : HIGH);

  delay(10); // ~100 Hz UI poll
}
