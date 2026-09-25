// =============================================================================
//  midi_task.h  –  MIDI record/playback FreeRTOS task (Core 0)
// =============================================================================
#pragma once
#include <stdint.h>

// Start the MIDI task pinned to Core 0 at priority 10.
// Call from setup() after Looper::init().
void midiTaskStart();

// Send a local CC value immediately and record a point when appropriate.
void midiSendLocalCC(uint8_t slot, uint8_t value);
