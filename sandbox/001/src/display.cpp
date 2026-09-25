// =============================================================================
//  display.cpp  -  monochrome v3 UI renderer, isolated from MIDI scheduling
// =============================================================================
#include "display.h"
#include "looper.h"
#include "ui.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <freertos/task.h>
#include <string.h>

#define SHARP_SCK D8
#define SHARP_MOSI D10
#define SHARP_SS D7
#define DISP_W 400
#define DISP_H 240
#define BLACK 0
#define WHITE 1

static Adafruit_SharpMem display(SHARP_SCK, SHARP_MOSI, SHARP_SS, DISP_W, DISP_H);
static const int TOP_H = 18;
static const int ROLL_Y = TOP_H + 2;
static const int ROLL_H = DISP_H - ROLL_Y;
static const int NOTE_LOW = 36;
static const int NOTE_HIGH = 96;

static int noteY(uint8_t pitch) {
  int value = constrain((int)pitch, NOTE_LOW, NOTE_HIGH);
  return ROLL_Y + ROLL_H - 1 - (value - NOTE_LOW) * (ROLL_H - 1) / (NOTE_HIGH - NOTE_LOW);
}

static int tickX(uint32_t tick) {
  uint32_t length = (uint32_t)Looper::settings.bars * TICKS_PER_BAR;
  return length == 0 ? 0 : (int)((uint64_t)tick * (DISP_W - 1) / length);
}

static void drawTopBar() {
  uint8_t count = UI::fieldCount();
  int width = DISP_W / (count == 0 ? 1 : count);
  char label[24];
  for (uint8_t i = 0; i < count; i++) {
    UI::fieldLabel(i, label, sizeof(label));
    int x = i * width;
    bool selected = UI::isFocused(i);
    if (selected) {
      display.fillRect(x, 0, width, TOP_H, BLACK);
      display.setTextColor(WHITE);
    } else display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(x + 2, 5);
    display.print(label);
    if (selected && UI::editing)
      display.drawFastHLine(x, TOP_H - 1, width, WHITE);
    if (i) display.drawFastVLine(x, 0, TOP_H, BLACK);
  }
  display.setTextColor(BLACK);
  display.drawFastHLine(0, TOP_H, DISP_W, BLACK);
}

static void drawGrid() {
  uint32_t length = (uint32_t)Looper::settings.bars * TICKS_PER_BAR;
  if (!Looper::settings.showGrid || length == 0) return;
  for (uint8_t bar = 1; bar < Looper::settings.bars; bar++) {
    int x = tickX((uint32_t)bar * TICKS_PER_BAR);
    for (int y = ROLL_Y; y < ROLL_Y + ROLL_H; y += 5) display.drawPixel(x, y, BLACK);
  }
}

static void drawRollFrame() {
  display.drawRect(0, ROLL_Y, DISP_W, ROLL_H, BLACK);
  for (int octave = 0; octave <= 5; octave++) {
    int y = noteY((uint8_t)(NOTE_LOW + octave * 12));
    for (int x = 0; x < DISP_W; x += 7) display.drawPixel(x, y, BLACK);
  }
}

static void drawNotes(const Note *notes, uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    int x1 = tickX(notes[i].startTick);
    int x2 = tickX(notes[i].startTick + notes[i].durationTick);
    if (x2 <= x1) x2 = x1 + 2;
    int y = noteY(notes[i].pitch);
    if (UI::selectedNote == (int16_t)i) display.fillRect(x1, y - 2, x2 - x1, 5, BLACK);
    else display.fillRect(x1, y - 1, x2 - x1, 3, BLACK);
  }
}

static void drawCurves(const CcCurve *curves) {
  for (uint8_t c = 0; c < CC_CURVE_COUNT; c++) {
    const CcCurve &curve = curves[c];
    if (curve.pointCount == 0 || curve.muted) continue;
    bool selected = c == Looper::activeCcCurve;
    int thickness = selected ? 2 : 1;
    for (uint16_t i = 1; i < curve.pointCount; i++) {
      int x1 = tickX(curve.points[i - 1].positionTick);
      int x2 = tickX(curve.points[i].positionTick);
      int y1 = ROLL_Y + ROLL_H - 1 - curve.points[i - 1].value * (ROLL_H - 1) / 127;
      int y2 = ROLL_Y + ROLL_H - 1 - curve.points[i].value * (ROLL_H - 1) / 127;
      display.drawLine(x1, y1, x2, y2, BLACK);
      if (thickness == 2) display.drawLine(x1, y1 + 1, x2, y2 + 1, BLACK);
    }
    if (curve.pointCount == 1) {
      int y = ROLL_Y + ROLL_H - 1 - curve.points[0].value * (ROLL_H - 1) / 127;
      display.drawFastHLine(0, y, DISP_W, BLACK);
    }
  }
}

static void drawPlayhead() {
  if (Looper::transport != TransportState::RUNNING) return;
  display.drawFastVLine(tickX(Looper::currentTick()), ROLL_Y, ROLL_H, BLACK);
}

static void drawSettings() {
  char label[24];
  uint8_t count = UI::fieldCount();
  int rowHeight = ROLL_H / (count > 1 ? count - 1 : 1);
  for (uint8_t i = 1; i < count; i++) {
    UI::fieldLabel(i, label, sizeof(label));
    int y = ROLL_Y + (i - 1) * rowHeight;
    if (UI::isFocused(i)) {
      display.fillRect(0, y, DISP_W, rowHeight - 1, BLACK);
      display.setTextColor(WHITE);
    } else display.setTextColor(BLACK);
    display.setCursor(5, y + 4);
    display.print(label);
  }
  display.setTextColor(BLACK);
}

static void renderFrame() {
  display.fillScreen(WHITE);
  drawTopBar();
  if (Looper::appMode == AppMode::SETTINGS) {
    drawSettings();
    return;
  }

  static Note noteSnapshot[MAX_NOTES];
  static CcCurve curveSnapshot[CC_CURVE_COUNT];
  uint16_t noteCount = 0;
  if (xSemaphoreTake(Looper::mutex, pdMS_TO_TICKS(4)) == pdTRUE) {
    noteCount = Looper::noteCount;
    memcpy(noteSnapshot, Looper::notes, noteCount * sizeof(Note));
    memcpy(curveSnapshot, Looper::ccCurves, sizeof(curveSnapshot));
    xSemaphoreGive(Looper::mutex);
  }
  drawRollFrame();
  drawGrid();
  if (Looper::appMode == AppMode::NOTE_LOOP || Looper::appMode == AppMode::NOTE_EDIT)
    drawNotes(noteSnapshot, noteCount);
  if (Looper::appMode == AppMode::CC_LOOP)
    drawCurves(curveSnapshot);
  drawPlayhead();
}

static void taskDisplay(void *) {
  display.begin();
  display.fillScreen(WHITE);
  display.refresh();
  for (;;) {
    renderFrame();
    display.refresh();
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}

void displayTaskStart() {
  xTaskCreatePinnedToCore(taskDisplay, "display", 8192, nullptr, 2, nullptr, 1);
}