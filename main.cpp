#include <Arduino.h>
#include <Control_Surface.h>

// ── Looper configuration ─────────────────────────────────────────────────────
static const uint32_t LOOP_MS    = 2000;   // loop duration in milliseconds
static const uint16_t MAX_EVENTS = 256;    // max recorded note events

// ── Recorded event ───────────────────────────────────────────────────────────
struct NoteEvent {
  uint32_t timestamp = 0; // ms into the loop (0 .. LOOP_MS-1)
  uint8_t  note = 0;
  uint8_t  velocity = 0;
  Channel  channel = Channel_1;
  bool     isNoteOn = false;
  bool     played = false;    // has been played in the current loop iteration?
};

// ── Circular ring buffer ──────────────────────────────────────────────────────
NoteEvent events[MAX_EVENTS];
uint16_t  evHead  = 0; // index of the oldest entry
uint16_t  evCount = 0; // number of valid entries

// Returns the event at logical index i (0 = oldest, evCount-1 = newest)
inline NoteEvent& getEvent(uint16_t i) {
  return events[(evHead + i) % MAX_EVENTS];
}

// Appends an event; drops the oldest one when the buffer is full
void addEvent(NoteEvent e) {
  uint16_t writeIdx = (evHead + evCount) % MAX_EVENTS;
  events[writeIdx] = e;
  if (evCount < MAX_EVENTS)
    evCount++;
  else
    evHead = (evHead + 1) % MAX_EVENTS; // overwrite oldest
}

// ── MIDI interface and callbacks ──────────────────────────────────────────────
USBMIDI_Interface midi;

uint32_t loopStart = 0;

struct LooperCallbacks : MIDI_Callbacks {
  void onChannelMessage(MIDI_Interface &, ChannelMessage msg) override {
    MIDIMessageType type = msg.getMessageType();
    bool isNoteOn  = (type == MIDIMessageType::NoteOn)  && (msg.data2 > 0);
    bool isNoteOff = (type == MIDIMessageType::NoteOff) ||
             ((type == MIDIMessageType::NoteOn) && (msg.data2 == 0));
    if (!isNoteOn && !isNoteOff)
      return;

    uint32_t ts = (millis() - loopStart) % LOOP_MS;
    addEvent({ts, msg.data1, msg.data2, msg.getChannel(), isNoteOn, true});
  }
} looperCB;

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {

  uint32_t ts = (millis() - loopStart) % LOOP_MS;
  addEvent({ts, 0x3C, 0x7F, Channel_1, true, true});
  ts = (ts + 50) % LOOP_MS;
  addEvent({ts, 0x3C, 0x00, Channel_1, false, true});

  midi.setCallbacks(looperCB);
  midi.begin();
  loopStart = millis();
  //setup built-in LED pin as output
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  midi.update(); // poll USB, triggers onChannelMessage for any incoming notes

  uint32_t now = millis();

  // ── Loop rollover ─────────────────────────────────────────────────────────
  if (now - loopStart >= LOOP_MS) {
    loopStart += LOOP_MS;
    // Reset the played flag so every event fires again next iteration
    for (uint16_t i = 0; i < evCount; i++)
      getEvent(i).played = false;
  }

  uint32_t pos = now - loopStart; // current position within the loop (ms)

  // ── Playback ──────────────────────────────────────────────────────────────
  for (uint16_t i = 0; i < evCount; i++) {
    NoteEvent &e = getEvent(i);
    if (!e.played && e.timestamp <= pos) {
        
        
        MIDIAddress addr = {e.note, e.channel};
        if (e.isNoteOn){
            digitalWrite(LED_BUILTIN, LOW); 
            midi.sendNoteOn(addr, e.velocity);
        } else {
            digitalWrite(LED_BUILTIN, HIGH); 
            midi.sendNoteOff(addr, e.velocity);
        }
        e.played = true;

    }
  }
}