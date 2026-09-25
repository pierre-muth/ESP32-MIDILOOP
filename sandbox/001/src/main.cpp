// =============================================================================
//  main.cpp  –  setup + loop (hardware init, task launch, UI polling)
// =============================================================================
#include <Arduino.h>
#include "looper.h"
#include "midi_task.h"
#include "display.h"
#include "ui.h"

// ── Hardware pins ─────────────────────────────────────────────────────────────
#define PIN_ENC_A    D1
#define PIN_ENC_B    D2
#define PIN_ENC_BTN  D3
#define PIN_BTN_REC  D4

// ── Encoder interrupt ─────────────────────────────────────────────────────────
volatile int16_t encoderDelta = 0;
static volatile int encLastA  = HIGH;

void IRAM_ATTR encISR() {
  int a = digitalRead(PIN_ENC_A);
  int b = digitalRead(PIN_ENC_B);
  if (a != encLastA) {
    encoderDelta += (a != b) ? 1 : -1;
    encLastA = a;
  }
}

// ── Debounce helper ───────────────────────────────────────────────────────────
static bool justPressed(int pin, bool &lastState) {
  bool cur   = (digitalRead(pin) == LOW);
  bool fired = cur && !lastState;
  lastState  = cur;
  return fired;
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
  // Debug UART: TX on D6, RX on D5 (only TX wire needed)
  Serial1.begin(115200, SERIAL_8N1, D5, D6);
  DBG_PRINTLN("\n\n[setup] MidiLooper booting...");

  // Core 0 runs a tight real-time loop — disable its watchdog
  disableCore0WDT();
  DBG_PRINTLN("[setup] Core 0 WDT disabled");

  pinMode(PIN_ENC_A,   INPUT_PULLUP);
  pinMode(PIN_ENC_B,   INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  pinMode(PIN_BTN_REC, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  encLastA = digitalRead(PIN_ENC_A);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);

  Looper::init();
  UI::init();

  midiTaskStart();
  displayTaskStart();

  DBG_PRINTLN("[setup] done");
}

// ── loop – UI polling @ ~100 Hz on Core 1 ────────────────────────────────────
void loop() {
  static bool lastRec    = false;
  static bool lastEncBtn = false;

  bool recPressed = justPressed(PIN_BTN_REC, lastRec);
  bool encPressed = justPressed(PIN_ENC_BTN, lastEncBtn);

  // Consume encoder ticks atomically
  int16_t delta = encoderDelta;
  encoderDelta  = 0;

  UI::update(delta, encPressed, recPressed);

  // Reflect recording state on built-in LED
  digitalWrite(LED_BUILTIN,
    Looper::recording == RecordingState::RECORD ? LOW : HIGH);

  delay(10); // 100 Hz
}