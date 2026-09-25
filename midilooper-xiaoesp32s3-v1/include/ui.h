// =============================================================================
//  ui.h - UI field abstraction, page navigation, focus/edit/blink state
// =============================================================================
#pragma once
#include <Arduino.h>
#include <functional>
#include "model.h"

enum class Page : uint8_t { LOOPER = 0, NOTE_EDIT, CC_EDIT, SETTINGS, COUNT };

enum class FieldType : uint8_t {
  MAIN,    // page-switch button (icon only)
  VALUE,   // numeric value, edit mode increments/decrements
  OPTION,  // enumeration, edit mode cycles through values
  LABEL,   // read-only text, or a momentary/toggle action
};

// A single UI field/button. The exact interface is an implementation detail;
// callbacks are plain std::function so pages can be built as static tables
// without touching the navigation/rendering code (description-v2.md section
// on the UI component interface).
struct Field {
  FieldType type;
  void (*getText)(char *buf, size_t len);
  void (*onEncoder)(int16_t delta);  // VALUE/OPTION: applied while editing
  void (*onClick)(void);              // LABEL: action performed on click
  bool (*isActive)(void);             // LABEL: sustained "activated" look (Run/Stop, Play/Record)
  bool momentary;                     // LABEL: blink 3x on click instead of sustained isActive
};

namespace UI {

  void init();

  // encoderDelta: accumulated encoder ticks since the last call (+CW/-CCW).
  // encClicked: true for exactly one call when the encoder button was pressed.
  void update(int16_t encoderDelta, bool encClicked);

  Page currentPage();
  uint8_t fieldCount();
  const Field &fieldAt(uint8_t idx);

  uint8_t focusIndex();
  bool isEditing();
  bool isBlinking(uint8_t idx);

  // Note Edit page selection (-1 = no note selected).
  int16_t selectedNoteId();

} // namespace UI
