// =============================================================================
//  display.cpp - monochrome UI renderer (buttons, chart, icons), core 1
// =============================================================================
#include "display.h"
#include "model.h"
#include "transport.h"
#include "midi_engine.h"
#include "ui.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
#include <algorithm>

static Adafruit_SharpMem display(PIN_SHARP_SCK, PIN_SHARP_MOSI, PIN_SHARP_SS, DISPLAY_W, DISPLAY_H);

// Approx. 5 octaves (60 note values) mapped over the full display height.
static const uint8_t NOTE_LOW = 36;
static const uint8_t NOTE_HIGH = 96;

int64_t startRenderLoopUs;

static int16_t noteY(uint8_t pitch) {
  int v = constrain((int)pitch, (int)NOTE_LOW, (int)NOTE_HIGH);
  return (int16_t)(DISPLAY_H - 1 - (v - NOTE_LOW) * (DISPLAY_H - 1) / (NOTE_HIGH - NOTE_LOW));
}

static int16_t tickX(uint32_t tick) {
  uint32_t length = Model::loopLengthTicks();
  if (length == 0) return CHART_X;
  return (int16_t)(CHART_X + (int32_t)((uint64_t)tick * (CHART_W - 1) / length));
}

static int16_t valueY(uint8_t value) {
  return (int16_t)(DISPLAY_H - 1 - (int32_t)value * (DISPLAY_H - 1) / 127);
}

static void ditherPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= DISPLAY_W || y >= DISPLAY_H) return;
  if (((x + y) & 1) == 0) display.drawPixel(x, y, color);
}

// ── Dashed grid lines (bar/beat graduations) ─────────────────────────────────
static void drawDashedVLine(int16_t x, const uint8_t *pattern, uint8_t len) {
  if (x < 0 || x >= DISPLAY_W) return;
  for (int16_t y = 0, i = 0; y < DISPLAY_H; y++, i = (i + 1) % len)
    if (pattern[i]) display.drawPixel(x, y, COLOR_BLACK);
}

// Bar/beat(/subdivision) dashed grid, shared by the Looper, Note Edit and CC
// Edit pages. While quantized, subdivision markers are added for grids finer
// than one beat (half/quarter/eighth).
static void drawBarBeatGrid() {
  uint32_t bars = Model::settings.bars;
  if (bars == 0) return;
  bool q = Model::quantMode != QuantMode::OFF;
  static const uint8_t barOn[2]   = { 1, 0 };
  static const uint8_t beatOn[3]  = { 1, 0, 0 };
  static const uint8_t subOn[4]   = { 1, 0, 0, 0 };
  static const uint8_t barOff[3]  = { 1, 0, 0 };
  static const uint8_t beatOff[5] = { 1, 0, 0, 0, 0 };

  uint32_t length = bars * TICKS_PER_BAR;
  uint32_t subStep = Model::quantStepTicks(Model::quantMode);
  if (q && subStep > 0 && subStep < TICKS_PER_BEAT) {
    for (uint32_t tick = 0; tick < length; tick += subStep) {
      if (tick % TICKS_PER_BEAT == 0) continue; // already drawn as a beat/bar line
      drawDashedVLine(tickX(tick), subOn, 4);
    }
  }

  for (uint32_t beat = 0; beat < bars * 4; beat++) {
    int16_t x = tickX(beat * TICKS_PER_BEAT);
    if (beat % 4 == 0) drawDashedVLine(x, q ? barOn : barOff, q ? 2 : 3);
    else drawDashedVLine(x, q ? beatOn : beatOff, q ? 3 : 5);
  }
}

// ── Notes ─────────────────────────────────────────────────────────────────────
static void drawNoteRect(const Note &n, uint32_t length) {
  if (length == 0 || n.startTick >= length) return;
  int16_t y = noteY(n.pitch);

  if (Model::quantMode == QuantMode::OFF) {
    uint32_t end = std::min(n.endTick(), length);
    int16_t x1 = tickX(n.startTick), x2 = tickX(end);
    if (x2 <= x1) x2 = x1 + 2;
    display.fillRect(x1, y - 2, x2 - x1, 4, COLOR_BLACK);
    return;
  }

  // Quantized: only the start snaps to the grid, duration is unchanged, so
  // the played note is the stored one translated by (quantized - raw) start.
  // An extension beyond the rectangle is shown as a thin black line, a
  // shortening as a thin white notch cut into the rectangle.
  uint32_t rawStart = n.startTick;
  uint32_t rawEnd = std::min(n.endTick(), length);
  uint32_t qStart = std::min(Model::quantizeTick(rawStart), length);
  uint32_t qEnd = std::min(qStart + n.durationTick, length);

  int16_t x1 = tickX(rawStart), x2 = tickX(rawEnd);
  if (x2 <= x1) x2 = x1 + 2;
  display.fillRect(x1, y - 2, x2 - x1, 4, COLOR_BLACK);

  if (qStart < rawStart) {
    int16_t lx1 = tickX(qStart), lx2 = tickX(rawStart);
    display.fillRect(lx1, y - 1, std::max(2, lx2 - lx1), 2, COLOR_BLACK);
  } else if (qStart > rawStart) {
    int16_t lx1 = tickX(rawStart), lx2 = tickX(std::min(qStart, rawEnd));
    display.fillRect(lx1, y - 1, std::max(2, lx2 - lx1), 2, COLOR_WHITE);
  }
  if (qEnd > rawEnd) {
    int16_t lx1 = tickX(rawEnd), lx2 = tickX(qEnd);
    display.fillRect(lx1, y - 1, std::max(2, lx2 - lx1), 2, COLOR_BLACK);
  } else if (qEnd < rawEnd) {
    int16_t lx1 = tickX(std::max(qEnd, rawStart)), lx2 = tickX(rawEnd);
    display.fillRect(lx1, y - 1, std::max(2, lx2 - lx1), 2, COLOR_WHITE);
  }
}

static void drawDitheredRoundRectOutline(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t thickness) {
  for (uint8_t t = 0; t < thickness; t++) {
    int16_t xx = x - t, yy = y - t, ww = w + 2 * t, hh = h + 2 * t;
    for (int16_t px = xx + r; px < xx + ww - r; px++) {
      ditherPixel(px, yy, COLOR_BLACK);
      ditherPixel(px, yy + hh - 1, COLOR_BLACK);
    }
    for (int16_t py = yy + r; py < yy + hh - r; py++) {
      ditherPixel(xx, py, COLOR_BLACK);
      ditherPixel(xx + ww - 1, py, COLOR_BLACK);
    }
    for (int a = 0; a <= 90; a += 6) {
      float rad = a * (float)M_PI / 180.0f;
      int16_t cs = (int16_t)lroundf(cosf(rad) * r), sn = (int16_t)lroundf(sinf(rad) * r);
      ditherPixel(xx + r - cs, yy + r - sn, COLOR_BLACK);
      ditherPixel(xx + ww - 1 - r + cs, yy + r - sn, COLOR_BLACK);
      ditherPixel(xx + r - cs, yy + hh - 1 - r + sn, COLOR_BLACK);
      ditherPixel(xx + ww - 1 - r + cs, yy + hh - 1 - r + sn, COLOR_BLACK);
    }
  }
}

static void drawSelectedNoteHighlight(const Note *notes, uint16_t count, uint32_t length) {
  int16_t sel = UI::selectedNoteId();
  if (sel < 0) return;
  for (uint16_t i = 0; i < count; i++) {
    if (notes[i].id != (uint8_t)sel) continue;
    if (notes[i].startTick >= length) return;
    uint32_t end = std::min(notes[i].endTick(), length);
    int16_t x1 = tickX(notes[i].startTick), x2 = tickX(end);
    if (x2 <= x1) x2 = x1 + 2;
    int16_t y = noteY(notes[i].pitch);
    drawDitheredRoundRectOutline(x1 - 4, y - 6, (x2 - x1) + 8, 12, 4, 3);
    return;
  }
}

// ── CC curves ─────────────────────────────────────────────────────────────────
static void drawDitheredLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t thickness) {
  if (x1 < x0) { std::swap(x0, x1); std::swap(y0, y1); }
  for (int16_t x = x0; x <= x1; x++) {
    int16_t y = (x1 == x0) ? y0 : (int16_t)(y0 + (int32_t)(y1 - y0) * (x - x0) / (x1 - x0));
    int16_t half = thickness / 2;
    for (int16_t t = -half; t < thickness - half; t++) ditherPixel(x, y + t, COLOR_BLACK);
  }
}

static void drawCurve(const CcCurve &c, bool solid, uint8_t thickness, uint32_t length) {
  if (c.pointCount == 0 || c.muted) return;
  if (c.pointCount == 1) {
    int16_t y = valueY(c.points[0].value);
    if (solid) for (uint8_t t = 0; t < thickness; t++) display.drawFastHLine(CHART_X, y + t, CHART_W, COLOR_BLACK);
    else drawDitheredLine(CHART_X, y, CHART_X + CHART_W - 1, y, thickness);
    return;
  }
  for (uint16_t i = 1; i < c.pointCount; i++) {
    if (c.points[i - 1].positionTick >= length) break;
    int16_t x1 = tickX(c.points[i - 1].positionTick);
    int16_t x2 = tickX(std::min(c.points[i].positionTick, length));
    int16_t y1 = valueY(c.points[i - 1].value);
    int16_t y2 = valueY(c.points[i].value);
    if (solid) for (uint8_t t = 0; t < thickness; t++) display.drawLine(x1, y1 + t, x2, y2 + t, COLOR_BLACK);
    else drawDitheredLine(x1, y1, x2, y2, thickness);
  }
}

// ── Right-hand MIDI input column ─────────────────────────────────────────────
static void drawLeftTriangle(int16_t xRight, int16_t y) {
  display.fillTriangle(xRight, y - 2, xRight, y + 2, xRight - 4, y, COLOR_BLACK);
}

static void drawRightColumn() {
  int16_t xRight = DISPLAY_W - 1;
  Page page = UI::currentPage();
  if (page == Page::LOOPER || page == Page::NOTE_EDIT) {
    for (int pitch = 0; pitch < 128; pitch++)
      if (MidiEngine::activeInputNote[pitch]) drawLeftTriangle(xRight, noteY((uint8_t)pitch));
  } else if (page == Page::CC_EDIT) {
    uint8_t idx = Model::selectedCurve;
    uint8_t value = Model::ccCurves[idx].inputMode == CcInputMode::MIDI_IN
                        ? MidiEngine::lastCcInputValue[idx]
                        : MidiEngine::localCcValue;
    drawLeftTriangle(xRight, valueY(value));
  }
}

// ── Mode icons (simplified glyphs) ───────────────────────────────────────────
static void drawLoopIcon(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t cx = x + w / 2, cy = y + h / 2;
  int16_t r = std::min(w, h) / 2 - 5;
  display.drawCircleHelper(cx, cy, r, 0x1, color);
  display.drawCircleHelper(cx, cy, r, 0x2, color);
  display.drawCircleHelper(cx, cy, r, 0x8, color);
  display.fillTriangle(cx + r + 3, cy - 1, cx + r - 4, cy - 6, cx + r - 4, cy + 4, color);
}

static void drawQuarterNoteIcon(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t headX = x + w / 2 - 5, headY = y + h / 2 + 7;
  int16_t stemH = std::min(h - 10, 22);
  display.fillCircle(headX, headY, 5, color);
  display.drawFastVLine(headX + 5, headY - stemH, stemH, color);
  display.drawLine(headX + 5, headY - stemH, headX + 11, headY - stemH + 6, color);
}

static void drawCcIcon(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  display.setTextColor(color);
  display.setTextSize(2);
  display.setCursor(x + w / 2 - 12, y + h / 2 - 8);
  display.print("CC");
}

static void drawCogIcon(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t cx = x + w / 2, cy = y + h / 2;
  int16_t r = std::min(w, h) / 2 - 8;
  display.drawCircle(cx, cy, r, color);
  display.drawCircle(cx, cy, r / 2, color);
  for (int a = 0; a < 360; a += 45) {
    float rad = a * (float)M_PI / 180.0f;
    int16_t x1 = cx + (int16_t)lroundf(cosf(rad) * r), y1 = cy + (int16_t)lroundf(sinf(rad) * r);
    int16_t x2 = cx + (int16_t)lroundf(cosf(rad) * (r + 4)), y2 = cy + (int16_t)lroundf(sinf(rad) * (r + 4));
    display.drawLine(x1, y1, x2, y2, color);
  }
}

static void drawModeIcon(Page page, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  switch (page) {
    case Page::LOOPER:    drawLoopIcon(x, y, w, h, color); break;
    case Page::NOTE_EDIT: drawQuarterNoteIcon(x, y, w, h, color); break;
    case Page::CC_EDIT:   drawCcIcon(x, y, w, h, color); break;
    case Page::SETTINGS:  drawCogIcon(x, y, w, h, color); break;
    default: break;
  }
}

// ── Buttons (left column on every page) ─────────────────────────────────────
static void buttonRect(uint8_t idx, uint8_t count, int16_t &x, int16_t &y, int16_t &w, int16_t &h) {
  w = LEFT_COLUMN_W;
  int16_t baseH = DISPLAY_H / count;
  x = 0;
  y = idx * baseH;
  h = (idx == count - 1) ? (DISPLAY_H - y) : baseH;
}

static void drawButtons() {
  Page page = UI::currentPage();
  uint8_t count = UI::fieldCount();
  char text[24];

  for (uint8_t i = 0; i < count; i++) {
    int16_t x, y, w, h;
    buttonRect(i, count, x, y, w, h);
    const Field &f = UI::fieldAt(i);
    bool selected = (i == UI::focusIndex());
    bool editingThis = selected && UI::isEditing();
    bool active = f.isActive ? f.isActive() : false;
    bool blink = UI::isBlinking(i);
    bool inverted = editingThis || active || blink;
    uint16_t fg = inverted ? COLOR_WHITE : COLOR_BLACK;

    if (inverted) display.fillRect(x, y, w, h, COLOR_BLACK);

    uint8_t thickness = selected ? 3 : 1;
    for (uint8_t t = 0; t < thickness; t++)
      display.drawRect(x + t, y + t, w - 2 * t, h - 2 * t, fg);

    if (i == 0) {
      drawModeIcon(page, x, y, w, h, fg);
    } else {
      f.getText(text, sizeof(text));
      display.setTextColor(fg);
      display.setTextSize(1);
      display.setCursor(x + 4, y + h / 2 - 4);
      display.print(text);
    }
  }
}

// ── Settings page: incoming-MIDI monitor (right of the button column) ────────
// Newest messages at the bottom; once the pane is full, older lines scroll up
// and out of the top. Line height for text size 1.
static const int16_t MON_X       = LEFT_COLUMN_W + 4;
static const int16_t MON_TITLE_H = 12;    // header height
static const int16_t MON_LINE_H  = 10;

static void formatMidiEntry(const Model::MidiLogEntry &e, char *b, size_t n) {
  char name[6], body[24];
  switch (e.type) {
    case Model::MidiLogType::NOTE_ON:
      Model::noteName(e.data1, name, sizeof(name));
      snprintf(body, sizeof(body), "ON  CH%2u %s V%u", e.channel, name, e.data2);
      break;
    case Model::MidiLogType::NOTE_OFF:
      Model::noteName(e.data1, name, sizeof(name));
      snprintf(body, sizeof(body), "OFF CH%2u %s V%u", e.channel, name, e.data2);
      break;
    case Model::MidiLogType::CC:
      snprintf(body, sizeof(body), "CC  CH%2u %3u=%3u", e.channel, e.data1, e.data2);
      break;
    default:
      snprintf(body, sizeof(body), "RAW CH%2u %3u %3u", e.channel, e.data1, e.data2);
      break;
  }
  // Timestamp of reception (seconds.milliseconds), fixed width so all
  // message columns line up regardless of uptime.
  snprintf(b, n, "%7lu.%03lu %s", (unsigned long)(e.ms / 1000), (unsigned long)(e.ms % 1000), body);
}

static void drawMidiMonitor() {
  display.setTextColor(COLOR_BLACK);
  display.setTextSize(1);
  display.setCursor(MON_X, 1);
  display.print("MIDI IN");
  display.drawFastHLine(LEFT_COLUMN_W, MON_TITLE_H - 3, DISPLAY_W - LEFT_COLUMN_W, COLOR_BLACK);

  static Model::MidiLogEntry entries[MIDI_LOG_SIZE];
  uint16_t count = Model::copyMidiLog(entries, MIDI_LOG_SIZE);

  char text[40];
  int16_t y = DISPLAY_H - MON_LINE_H;
  display.setTextColor(COLOR_BLACK);
  for (int16_t i = (int16_t)count - 1; i >= 0 && y >= MON_TITLE_H; i--, y -= MON_LINE_H) {
    formatMidiEntry(entries[i], text, sizeof(text));
    display.setCursor(MON_X, y);
    display.print(text);
  }
}

// ── Frame assembly ────────────────────────────────────────────────────────────
static void renderFrame() {
  display.clearDisplayBuffer();
  
  Page page = UI::currentPage();

  if (page == Page::SETTINGS) {
    drawButtons();
    drawMidiMonitor();
    display.refresh();
    return;
  }

  static Note noteSnapshot[MAX_NOTES];
  static CcCurve curveSnapshot[CC_CURVE_COUNT];
  uint16_t noteCountSnap = 0;
  if (xSemaphoreTake(Model::mutex, pdMS_TO_TICKS(4)) == pdTRUE) {
    noteCountSnap = Model::noteCount;
    memcpy(noteSnapshot, Model::notes, noteCountSnap * sizeof(Note));
    memcpy(curveSnapshot, Model::ccCurves, sizeof(curveSnapshot));
    xSemaphoreGive(Model::mutex);
  }
  uint32_t length = Model::loopLengthTicks();

  if (page == Page::LOOPER) {
    drawBarBeatGrid();
    for (uint8_t i = 0; i < CC_CURVE_COUNT; i++) drawCurve(curveSnapshot[i], false, 3, length);
    for (uint16_t i = 0; i < noteCountSnap; i++) drawNoteRect(noteSnapshot[i], length);
  } else if (page == Page::NOTE_EDIT) {
    drawBarBeatGrid();
    for (uint16_t i = 0; i < noteCountSnap; i++) drawNoteRect(noteSnapshot[i], length);
    drawSelectedNoteHighlight(noteSnapshot, noteCountSnap, length);
  } else if (page == Page::CC_EDIT) {
    drawBarBeatGrid();
    for (uint8_t i = 0; i < CC_CURVE_COUNT; i++)
      if (i != Model::selectedCurve) drawCurve(curveSnapshot[i], false, 3, length);
    drawCurve(curveSnapshot[Model::selectedCurve], true, 3, length);
  }

  if (Transport::state == TransportState::RUNNING)
    display.drawFastVLine(tickX(Transport::currentTick()), 0, DISPLAY_H, COLOR_BLACK);

  drawRightColumn();
  drawButtons();
  display.refresh();
}

static void taskDisplay(void *) {
  esp_timer_get_time(); // warm up the timer to avoid a long first call
  display.begin();
  display.clearDisplayBuffer();
  display.refresh();

  float fps;
  int spiSpeed;
  startRenderLoopUs = (int64_t)esp_timer_get_time();

  for (;;) {
    fps = (float)(1000000.0f / (float)(esp_timer_get_time() - startRenderLoopUs));
    startRenderLoopUs = (int64_t)esp_timer_get_time();
    renderFrame();
    // DBG_PRINTLN("[taskDisplay] FPS: "+String(fps)+", Render time: "+String((int64_t)esp_timer_get_time() - startRenderLoopUs)+" us");
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void displayTaskStart() {
  xTaskCreatePinnedToCore(taskDisplay, "display", 8192, nullptr, 3, nullptr, 1);
}
