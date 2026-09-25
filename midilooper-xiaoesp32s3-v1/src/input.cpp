// =============================================================================
//  input.cpp - quadrature encoder ISR and debounced button reads
// =============================================================================
#include "input.h"
#include "config.h"

namespace Input {

static volatile int16_t encoderRaw = 0; // 2 counts per detent (both edges of A)
static volatile int encLastA = HIGH;
static int16_t encoderRemainder = 0;    // leftover half-step, carried between reads

static bool lastEncBtn = false;
static bool lastRunStop = false;

// CHANGE fires on both edges of A per detent (2 raw counts/detent); this is
// more bounce-tolerant than a single-edge trigger, so the /2 scaling to a
// logical step happens in consumeEncoderDelta() instead.
static void IRAM_ATTR encoderIsr() {
  int a = digitalRead(PIN_ENC_A);
  int b = digitalRead(PIN_ENC_B);
  if (a != encLastA) {
    encoderRaw += (a != b) ? 1 : -1;
    encLastA = a;
  }
}

void init() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  pinMode(PIN_RUN_STOP, INPUT_PULLUP);
  encLastA = digitalRead(PIN_ENC_A);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderIsr, CHANGE);
}

int16_t consumeEncoderDelta() {
  noInterrupts();
  int16_t raw = encoderRaw;
  encoderRaw = 0;
  interrupts();

  int16_t total = encoderRemainder + raw;
  int16_t steps = total / 2;
  encoderRemainder = total - steps * 2;
  return steps;
}

static bool justPressed(int pin, bool &lastState) {
  bool cur = digitalRead(pin) == LOW;
  bool fired = cur && !lastState;
  lastState = cur;
  return fired;
}

bool encoderButtonPressed() { return justPressed(PIN_ENC_BTN, lastEncBtn); }
bool runStopButtonPressed() { return justPressed(PIN_RUN_STOP, lastRunStop); }

} // namespace Input
