// =============================================================================
//  display.h - display task interface
// =============================================================================
#pragma once

// Starts the display/UI-rendering FreeRTOS task (core 1, low priority).
// Call from setup() after Model::init(), Transport::init() and UI::init().
void displayTaskStart();
