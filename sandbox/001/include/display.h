// =============================================================================
//  display.h  –  display task interface
// =============================================================================
#pragma once

// Initialise and start the display FreeRTOS task (Core 1, priority 2).
// Call from setup() after Looper::init().
void displayTaskStart();
