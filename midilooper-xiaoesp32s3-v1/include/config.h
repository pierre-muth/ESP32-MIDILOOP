// =============================================================================
//  config.h - pin map, capacities, musical resolution, debug helpers
// =============================================================================
#pragma once
#include <Arduino.h>

// ── Physical pins ─────────────────────────────────────────────────────────────
#define PIN_ENC_A      D1
#define PIN_ENC_B      D2
#define PIN_ENC_BTN    D3
#define PIN_RUN_STOP   D4
#define PIN_DEBUG_RX   D5
#define PIN_DEBUG_TX   D6
#define PIN_SHARP_SS   D7
#define PIN_SHARP_SCK  D8
#define PIN_SHARP_MOSI D10

// ── Debug UART (Serial1, TX only wired) ───────────────────────────────────────
#define DBG Serial1
#define DBG_BAUD 115200
#define DBG_PRINTLN(x)   DBG.println(x)
#define DBG_PRINTF(...)  DBG.printf(__VA_ARGS__)

// ── Musical timing resolution ─────────────────────────────────────────────────
// Fixed-point ticks avoid accumulating float error; fractional-bar positions are
// derived from these on demand (see Model helpers). 1000/beat keeps both the
// beat (quantized) and hundredth-of-bar (non-quantized) edit steps exact integers.
static const uint32_t TICKS_PER_BEAT = 1000;
static const uint32_t TICKS_PER_BAR  = TICKS_PER_BEAT * 4;

// ── Capacities ────────────────────────────────────────────────────────────────
static const uint16_t MAX_NOTES      = 128;
static const uint16_t MAX_CC_POINTS  = 256;
static const uint8_t  CC_CURVE_COUNT = 4;

// ── Valid ranges ──────────────────────────────────────────────────────────────
static const uint8_t  MIN_BARS        = 1;
static const uint8_t  MAX_BARS        = 64;
static const uint16_t MIN_BPM         = 20;
static const uint16_t MAX_BPM         = 300;
static const uint8_t  MIN_CC_RATE_HZ  = 1;
static const uint8_t  MAX_CC_RATE_HZ  = 200;
static const uint8_t  MIN_MIDI_CHANNEL = 1;
static const uint8_t  MAX_MIDI_CHANNEL = 16;
static const uint8_t  MIN_CC_KNOB_SENS = 1;
static const uint8_t  MAX_CC_KNOB_SENS = 10;

// ── MIDI input monitor (Settings page) ───────────────────────────────────────
static const uint8_t MIDI_LOG_SIZE = 64;

// ── Display geometry (400x240 Sharp Memory LCD) ───────────────────────────────
static const int16_t DISPLAY_W = 400;
static const int16_t DISPLAY_H = 240;
// Left button column ~25% of width, far-right MIDI input indicator column 5px.
static const int16_t LEFT_COLUMN_W  = 100;
static const int16_t RIGHT_COLUMN_W = 5;
static const int16_t CHART_X = LEFT_COLUMN_W;
static const int16_t CHART_W = DISPLAY_W - LEFT_COLUMN_W - RIGHT_COLUMN_W;

// Monochrome colors as used by Adafruit_SharpMem (0 = black, 1 = white).
static const uint16_t COLOR_BLACK = 0;
static const uint16_t COLOR_WHITE = 1;
