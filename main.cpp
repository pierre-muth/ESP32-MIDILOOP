#include <Arduino.h>
#include <Control_Surface.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>

// any pins can be used
#define SHARP_SCK  13
#define SHARP_MOSI 11
#define SHARP_SS   10


#define BLACK 0
#define WHITE 1

Adafruit_SharpMem display(SHARP_SCK, SHARP_MOSI, SHARP_SS, 400, 240);

USBMIDI_Interface midi;
NoteLED led { LED_BUILTIN, MIDI_Notes::C[4] };

void setup() { 
  Control_Surface.begin();
 }

// Specify the MIDI note number to trigger, and the velocity with which to 
// trigger it
const MIDIAddress note = MIDI_Notes::C[4]; // C4 is middle C
const uint8_t velocity = 127;              // 127 is maximum velocity
 
void loop() {
    // Send a MIDI Note On message for the given note to trigger it
    midi.sendNoteOn(note, velocity);  
    delay(50);
    // Send a MIDI Note Off message to turn it off again
    midi.sendNoteOff(note, velocity);
    delay(500);
    
    midi.update();
}