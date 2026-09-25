# USB MIDI Looper — Project Specification

## 1. Project overview

This project is a small USB MIDI looper built around a Seeed Studio XIAO ESP32-S3 running Arduino/PlatformIO.

The device is intended to sit directly between a USB MIDI synthesizer and the looper. A typical setup is:

```text
USB MIDI synthesizer
        │
        │ USB MIDI
        ▼
   MIDI Looper
```

For example, the looper can be connected directly to a Teenage Engineering OP-1 Field or another synthesizer supporting USB MIDI. The looper may therefore also be powered by the synthesizer.

The looper:

- records MIDI notes into a loop;
- plays recorded notes back repeatedly;
- supports overdubbing;
- supports four MIDI CC automation curves;
- allows the CC curves to be edited with a rotary encoder;
- provides a monochrome graphical UI;
- uses a physical Run/Stop button for immediate transport control.

The first version is deliberately focused on a single loop and a simple, self-contained workflow.

---

## 2. Implementation contract

The behavior described in this document is authoritative.

The implementation must separate:

- loop data/model;
- transport and timing;
- MIDI input processing;
- MIDI output scheduling;
- recording/overdubbing;
- UI state and rendering;
- physical hardware input.

Timing-critical MIDI event scheduling must have deterministic timing and must not be blocked by display rendering.

The UI must not implement MIDI playback or recording logic directly. UI actions should modify the appropriate model or send commands to the looper/transport layer.

The implementation should use reusable abstractions rather than duplicating page-specific logic.

When implementation details are not specified by this document, the coding agent may choose a suitable implementation. It should not invent new user-visible behavior without documenting it.

When the specification is ambiguous about user-visible behavior, the ambiguity should be identified rather than silently introducing arbitrary behavior.

---

## 3. Scope

### 3.1 Supported functionality

The first version supports:

- one MIDI loop;
- configurable loop length in bars;
- configurable BPM;
- MIDI note recording and playback;
- note overdubbing;
- note editing;
- four independent MIDI CC automation curves;
- CC curve recording from MIDI input;
- CC curve recording from the rotary encoder;
- CC curve editing;
- independent mute state for the four CC curves;
- configurable CC output rate;
- quantized and non-quantized note editing;
- monochrome graphical UI;
- physical Run/Stop transport button.

### 3.2 Non-goals for the first version

The first version does not need to support:

- multiple simultaneous loops;
- MIDI clock synchronization;
- MIDI clock input/output;
- external tempo synchronization;
- time signatures other than 4/4;
- swing/groove;
- note transposition;
- undo/redo;
- saving loops to SD card;
- loading loops from SD card;
- persistent loop storage;
- multiple independent MIDI input/output ports.

These may be considered future functionality.

Possible future features include:

- MIDI clock synchronization;
- SD card loop storage;
- multiple loops/scenes;
- external MIDI synchronization;
- additional MIDI automation types.

---

# 4. Hardware

## 4.1 Main controller

- Seeed Studio XIAO ESP32-S3
- Arduino framework
- PlatformIO project

## 4.2 MIDI

USB MIDI is implemented using:

- `tttapa/Control Surface`

The looper is connected directly to a USB MIDI synthesizer.

The MIDI implementation must support:

- MIDI input from the synthesizer;
- MIDI output to the synthesizer;
- MIDI note messages;
- MIDI CC messages.

### MIDI channel handling

For recorded notes, the input MIDI channel is ignored.

Notes are identified by their pitch rather than by their original MIDI channel.

The looper has a configurable **MIDI output channel** setting. All generated MIDI notes and CC messages are sent using this configured output MIDI channel.

A CC curve additionally has a configurable **CC number** from 0 to 127.

Terminology:

- MIDI channel: 1..16;
- CC number: 0..127;
- CC value: 0..127.

The UI and code should not call the CC number a "CC channel".

## 4.3 Physical controls

### Run / Stop button

- Connected to D4.
- Starts or stops the loop.

### Rotary encoder

- Encoder A: D1
- Encoder B: D2
- Encoder push button: D3

### Debug UART

- TX: D6
- RX: D5
- Only TX is required.
- Baud rate: 115200.

### Built-in LED

- Connected to `LED_BUILTIN`.

## 4.4 Display

Monochrome Sharp Memory LCD.

Library:

- `adafruit/Adafruit SHARP Memory Display`

SPI:

```text
SHARP_SCK  = D8
SHARP_MOSI = D10
SHARP_SS   = D7
```

Resolution:

```text
400 x 240 pixels
```

Approximate viewing area:

```text
58 mm x 35 mm
```

The display therefore has relatively high pixel density.

Drawing can use `Adafruit_GFX`.

Avoid calling `clearDisplay()` as part of the normal drawing process because the display immediately becomes white and this can cause visible flicker.

---

# 5. Core musical timing model

## 5.1 Musical meter

The looper always uses:

```text
4/4 time
```

Therefore:

```text
1 bar = 4 beats
1 beat = one quarter note
```

The BPM represents beats per minute.

For example, at 120 BPM:

```text
1 beat = 0.5 seconds
1 bar = 2 seconds
```

The loop length is expressed in whole bars.

## 5.2 Internal musical position

Loop positions are conceptually represented as a fractional number of bars.

Examples:

```text
0.0   = beginning of loop
0.25  = one quarter of the first bar
1.0   = beginning of bar 2
2.75  = three quarters through bar 3
```

This representation is used for note timing and CC curve points.

This is important because changing BPM must change the playback duration of the musical positions without changing the recorded musical structure.

Changing BPM therefore does not require converting all recorded events from milliseconds to new timestamps.

### Implementation note

The conceptual model uses fractional musical positions, but the implementation should avoid accumulating floating-point timing errors.

The implementation should therefore use a deterministic high-resolution internal representation, such as fixed-point musical ticks, while exposing musical positions conceptually as fractional bars.

The exact internal tick resolution is an implementation detail unless required by the MIDI timing implementation.

## 5.3 Loop duration

For a loop of `N` bars:

```text
loop duration in beats = N * 4
loop duration in seconds = N * 4 * 60 / BPM
```

Playback position wraps at the loop boundary.

## 5.4 BPM changes

BPM can be changed while the loop is playing.

Changing BPM changes playback speed immediately.

Recorded musical positions do not change.

BPM cannot be changed while recording notes or CC data.

---

# 6. Core data model

The exact C++ classes and containers are implementation details, but the conceptual model is:

```text
Loop
    lengthBars
    notes[]
    ccCurves[4]
```

## 6.1 Note

Each note contains:

```text
id
pitch       0..127
velocity    0..127
start       musical position in bars
duration    musical duration in bars
```

The note end position is:

```text
end = start + duration
```

No MIDI note may remain active across a loop boundary.

A note whose end would extend beyond the loop must therefore be handled so that no active MIDI note survives the loop boundary.

## 6.2 Note uniqueness

Two stored notes cannot have the same pitch as separate notes.

If a recording or editing operation would create/modify a note with a pitch that already exists, the notes are merged according to the overdubbing rules defined below.

## 6.3 CC curve

There are exactly four CC curves.

Each curve contains:

```text
midiChannel
ccNumber
muted
points[]
```

Each point contains:

```text
position    musical position in bars
value       0..127
```

The four curves are independent.

By default:

- curve 1 uses CC number 1;
- curve 2 uses CC number 2;
- curve 3 uses CC number 3;
- curve 4 uses CC number 4;
- all curves are muted.

The output MIDI channel is controlled globally by the MIDI output channel setting.

---

# 7. Transport state machine

The transport and recording behavior should be treated as an explicit state machine.

## 7.1 Transport state

The transport has two primary states:

```text
STOPPED
RUNNING
```

The physical Run/Stop button toggles between these states.

### STOPPED

- The playhead is at loop position 0.
- No playback events are generated.
- All active MIDI notes must be turned off.
- Recording can be armed.

### RUNNING

- The playhead advances continuously through the loop.
- Recorded notes are played.
- CC curves are output according to their state.
- The playhead wraps to zero at the loop boundary.

Stopping always returns the playhead to zero.

## 7.2 Recording state

Recording is a separate state from transport.

```text
PLAY
RECORD
ARMED
```

`ARMED` is only used while the transport is stopped.

### PLAY

Normal playback.

No recording takes place.

### RECORD

Recording/overdubbing is active while the transport is running.

### ARMED

The transport is stopped and recording has been armed.

The next incoming recordable event starts the loop automatically.

For note recording, the first incoming MIDI Note On starts the loop and enters RECORD.

For CC recording, the first recordable CC input or rotary encoder movement starts the loop and enters RECORD, depending on the selected CC input mode.

## 7.3 State transition table

### Run/Stop button

```text
STOPPED + PLAY
    -> RUNNING + PLAY

STOPPED + ARMED
    -> RUNNING + ARMED/RECORD behavior as defined by the active recording mode

RUNNING + PLAY
    -> STOPPED + PLAY

RUNNING + RECORD
    -> STOPPED + PLAY

RUNNING + any recording state
    -> STOPPED + PLAY
```

Whenever the transport enters STOPPED:

- playhead returns to zero;
- all active MIDI notes receive Note Off;
- recording state returns to PLAY.

## 7.4 Note recording transitions

```text
STOPPED + PLAY
    encoder selects Record
        -> STOPPED + ARMED

STOPPED + ARMED
    first MIDI Note On
        -> RUNNING + RECORD

STOPPED + ARMED
    user exits recording
        -> STOPPED + PLAY

RUNNING + PLAY
    user selects Record
        -> RUNNING + RECORD

RUNNING + RECORD
    user exits recording
        -> RUNNING + PLAY
```

When recording starts while the loop is already running, the loop continues from its current position.

Input notes are recorded but are not echoed immediately to the MIDI output.

The recorded note is played on the next appropriate loop iteration.

## 7.5 Note recording stop behavior

If recording stops while a MIDI note is currently held:

- the note-off timing is recorded at the moment recording stops;
- the resulting note is stored normally.

---

# 8. Note recording and overdubbing

## 8.1 Basic recording

A MIDI Note On records:

```text
pitch
velocity
start position
```

A corresponding MIDI Note Off records:

```text
end position
```

The duration is derived from the start and end positions.

Input notes are not echoed immediately during recording.

## 8.2 Overdubbing

Recording while the loop is already running adds/merges notes into the existing loop.

The rule for notes of the same pitch is:

> The resulting note uses the earliest start and latest end of all overlapping notes being merged.

For example:

```text
Existing:
        |---------|
New:
    |---------------|

Result:
    |---------------|
```

If a newly recorded note overlaps two existing notes of the same pitch:

```text
Existing:    |----|       |----|
New:             |------------|
```

the resulting note uses the widest boundaries:

```text
Result:      |-----------------|
```

The merge operation therefore uses:

```text
mergedStart = minimum(all involved starts)
mergedEnd   = maximum(all involved ends)
```

The resulting duration is:

```text
mergedDuration = mergedEnd - mergedStart
```

There must never be two independent stored notes with the same pitch.

## 8.3 Velocity when merging

When multiple notes are merged, the implementation must preserve a single velocity value.

The default rule is:

```text
use the velocity of the newly recorded Note On
```

If this rule is changed later, it should be changed centrally in the note merge logic.

---

# 9. Loop boundary behavior

A MIDI note must never remain active across a loop boundary.

At every loop boundary:

1. Send Note Off for every currently active output note.
2. Reset the active-note tracking.
3. Move the playhead to position 0.
4. Schedule notes that begin at position 0.

The loop data itself remains unchanged.

A note whose nominal end extends beyond the loop boundary must be handled so that its output is stopped at the boundary.

---

# 10. Quantization

Quantization is a user option affecting note playback/editing representation.

## 10.1 Quantization ON

When enabled:

- note timing is represented using whole-bar steps for editing;
- note start and duration values are changed in whole-bar increments;
- chart bar graduations are displayed every bar;
- note playback timing is aligned to the corresponding quantized musical positions.

The chart displays the quantized timing and the relationship between the stored note and the quantized trigger boundaries as described in the UI section.

## 10.2 Quantization OFF

When disabled:

- note timing uses continuous/high-resolution musical positions;
- chart bar graduations are displayed every 8 bars;
- notes are displayed at their actual musical positions.

The previous "tenths of bars" editing restriction is intentionally removed.

Non-quantized timing must be represented with sufficient resolution to allow precise recording and editing.

---

# 11. MIDI playback

The playback engine continuously tracks the current musical position in the loop.

For notes:

- Note On is generated when the playhead reaches the note start.
- Note Off is generated when the playhead reaches the note end.
- Events must be generated at deterministic timing.
- Output uses the configured global MIDI output channel.

Playback must not depend on display rendering frequency.

The display may update at approximately 20 FPS while MIDI scheduling runs independently.

---

# 12. CC automation

The looper supports four independent CC curves.

## 12.1 CC point model

A CC curve is represented as a set of points:

```text
(position in bars, value 0..127)
```

For example:

```text
(0.00, 20)
(0.50, 80)
(1.25, 100)
(2.75, 40)
```

The curve is considered continuous between points.

## 12.2 Interpolation

Between two points, the output value is linearly interpolated.

For a position between:

```text
P1 = (time1, value1)
P2 = (time2, value2)
```

the output value is interpolated between `value1` and `value2`.

The output value is always clamped to:

```text
0..127
```

## 12.3 Zero and one point

If a curve contains zero points:

```text
no CC messages are sent for this curve
```

If a curve contains exactly one point:

```text
the same CC value is output for the entire loop
```

## 12.4 Before and after the curve

The curve is continuous over the complete loop.

Before the first point and after the last point, the nearest point value is held.

Therefore:

- before the first point: hold first value;
- after the last point: hold last value.

At the loop boundary, the curve wraps and starts again from the beginning of the loop.

## 12.5 CC output rate

CC values are sent at a configurable rate.

Default:

```text
50 Hz
```

The rate is changed in Settings.

Only curves that:

- contain at least one point;
- are not muted;

generate CC output.

## 12.6 CC muting

Each curve can be muted independently.

When a curve is muted while the loop is running:

- stop sending that CC number immediately;
- do not modify the stored curve data.

When the curve is unmuted, output resumes according to the curve's current value.

---

# 13. CC recording

The CC editor supports two input modes:

```text
MIDI IN
LOCAL
```

## 13.1 MIDI IN

Only incoming MIDI CC messages corresponding to the selected curve are recorded.

The selected curve determines which CC number is being recorded.

When recording:

- incoming values replace/update the curve at the current playhead position;
- the MIDI input is not echoed directly to the output as a separate live MIDI event.

## 13.2 LOCAL

The rotary encoder is used to create/update CC curve points.

During recording:

- rotating the encoder changes the selected curve value at the current playhead position;
- the encoder controls the value being recorded;
- curve points are generated/updated as required by the implementation.

When recording is started in LOCAL mode while the loop is already running, the current curve value is initially followed until the user moves the encoder. After encoder movement, the local input overrides the curve at the current position.

---

# 14. CC recording state behavior

If CC recording is activated while the loop is stopped:

1. Enter ARMED state.
2. Wait for the first recordable input:
   - MIDI CC in MIDI IN mode;
   - encoder movement in LOCAL mode.
3. Start the loop.
4. Enter RECORD.
5. Record for one complete loop cycle.
6. At the end of that loop cycle, automatically return to PLAY.
7. Continue looping normally.

If CC recording is activated while the loop is already running:

- recording begins immediately;
- behavior continues according to the selected MIDI IN / LOCAL mode.

The exact point-management algorithm may be implemented internally, but the resulting curve must represent the recorded value over musical position.

---

# 15. User interface architecture

The UI uses a reusable button abstraction.

Buttons are rectangles containing text or icons.

Normal state:

```text
outline = 1 pixel
```

Selected state:

```text
outline = 3 pixels
```

Activated/active state:

```text
drawing colors are inverted
```

Only one UI button is selected at a time.

When no button is in an active editing state:

- rotating the encoder moves selection through the available buttons.

Clicking the encoder on the selected button activates it.

## 15.1 Generic button types

### Value

A numerical value.

Behavior:

1. Click → enter edit mode.
2. Rotate → increment/decrement.
3. Click again → leave edit mode.

The value must be clamped to its defined minimum/maximum.

### Option

An enumeration.

Behavior:

1. Click → enter edit mode.
2. Rotate → cycle through allowed values.
3. Click again → leave edit mode.

### Main mode button

The first button on every page.

Clicking it switches to the next UI page.

It displays only the page/mode icon.

### Label

A read-only label.

It may display:

- text;
- a number;
- a status.

A Label can also perform an action when clicked.

After activation it gives visual feedback by inverting/blinking three times.

Rotation has no effect on a Label.

## 15.2 UI component interface

The implementation should provide a reusable UI component abstraction supporting concepts such as:

```text
type
bounds
label/icon
selected
editing/active state
draw()
onClick()
onEncoder(delta)
```

The exact C++ interface is an implementation detail.

Adding a button to a page should not require rewriting the page navigation system.

---

# 16. UI modes

There are four pages:

1. Looper
2. Note Edit
3. CC Edit
4. Settings

The main mode button is always the first button.

Page icons:

```text
Looper    = looping arrow
Note Edit = black quarter note
CC Edit   = "CC"
Settings  = cog
```

The Settings page is the last page.

Clicking the Settings mode button returns to Looper.

---

# 17. Looper page

The page is divided approximately as:

```text
25%   left controls
75%   note chart
5 px  MIDI input indicator column at far right
```

## 17.1 Buttons

Below the main mode button:

1. Label: Run / Stop status
2. Label: Play / Record / Armed
3. Option: Quantization On / Off
4. Value: BPM
5. Label: loop length in bars

Loop length is read-only here and is edited in Settings.

## 17.2 Run / Stop

The physical D4 button controls transport.

When running:

- notes are played;
- CC automation is output;
- playhead is animated.

When stopped:

- playhead returns to zero;
- all active notes receive Note Off;
- recording state returns to PLAY.

## 17.3 Play / Record

Clicking toggles recording behavior.

### Running

```text
PLAY -> RECORD
RECORD -> PLAY
```

In RECORD:

- MIDI notes are recorded/overdubbed;
- input notes are not echoed immediately.

### Stopped

```text
PLAY -> ARMED
ARMED -> PLAY
```

The first MIDI Note On while ARMED starts the loop and enters RECORD.

## 17.4 BPM

BPM can be edited while not recording.

BPM changes affect playback speed immediately.

BPM cannot be changed while recording.

## 17.5 Note chart

The chart occupies approximately 75% of the screen width.

The horizontal axis represents the complete loop.

A vertical animated playhead is displayed while the loop is running.

Notes are filled horizontal rectangles.

- rectangle horizontal start = note start;
- rectangle horizontal end = note end;
- rectangle height = 4 pixels;
- vertical position = MIDI pitch.

The vertical span should represent approximately 5 octaves / 60 MIDI note values over the 240-pixel display height.

## 17.6 Quantized chart

When quantization is enabled:

- bar boundaries are shown as light dashed vertical lines;
- notes show their relationship to quantized trigger positions as specified by the quantization representation.

## 17.7 Non-quantized chart

When quantization is disabled:

- dashed vertical lines are shown every 8 bars;
- notes are drawn at their actual musical positions.

## 17.8 CC background

The chart background may show the four CC curves using a gray/chessboard representation.

Only enabled/unmuted curves are shown.

## 17.9 MIDI input column

A 5-pixel column at the far right displays incoming MIDI notes.

Each active incoming note is represented by a small 4-pixel triangle pointing left.

The triangle appears on Note On and disappears on Note Off.

---

# 18. Note Edit page

This page edits individual recorded notes.

Layout is the same as the Looper page:

```text
25% controls
75% note chart
5 px MIDI input column
```

## 18.1 Buttons

1. Value: ID
2. Option: Note pitch/name
3. Value: start
4. Value: duration
5. Value: velocity
6. Option: Quantization On / Off
7. Label: New
8. Label: Delete

## 18.2 Selected note

The note selected by ID is surrounded by:

```text
3-pixel rounded rectangle
gray/chessboard pattern
```

Changing the ID cycles through the existing notes.

If there are zero notes:

- there is no selected note;
- note-editing controls have no effect;
- Delete has no effect.

## 18.3 Editing

Changing:

- pitch → changes vertical position;
- start → changes horizontal start;
- duration → changes horizontal end;
- velocity → changes velocity only.

When quantization is ON:

- start and duration are edited in whole-bar increments;
- chart graduations are every bar.

When quantization is OFF:

- start and duration use high-resolution fractional-bar positions;
- chart graduations are every 8 bars.

Encoder values are always clamped to their valid minimum/maximum.

## 18.4 New

Clicking New:

- creates a new note;
- assigns a new ID;
- selects it;
- uses the last received MIDI Note On pitch;
- places it at the same bar as the last selected note;
- gives it a default duration of one bar.

The note must obey the unique-pitch rule and therefore may merge with an existing note of the same pitch.

## 18.5 Delete

Clicking Delete:

- removes the selected note;
- selects the previous note ID if one exists.

If no notes remain, there is no selection.

---

# 19. CC Edit page

This page edits the four CC curves.

## 19.1 Buttons

1. Label: Run / Stop status
2. Option: selected curve 1..4
3. Label: Play / Record / Armed
4. Option: MIDI IN / LOCAL
5. Value: BPM
6. Option: Muted On / Off
7. Value: CC number 0..127
8. Label: Clear curve
9. Label: loop length in bars

Loop length is read-only here and is edited in Settings.

BPM follows the same rules as the Looper page and cannot be changed while recording.

## 19.2 Curve selection

Changing selected curve:

- changes the highlighted curve;
- changes which curve is modified;
- changes the selected CC number and mute state shown by the controls.

## 19.3 Curve display

The selected curve:

- 3 pixels thick;
- solid.

Other curves:

- 3 pixels thick;
- gray/chessboard representation.

## 19.4 MIDI input display

The far-right column shows the selected curve's input value.

In MIDI IN mode:

- show the latest incoming MIDI CC value for the selected CC number.

In LOCAL mode:

- while recording, the value is driven by the rotary encoder.

---

# 20. Settings page

The Settings page uses the full display width for its controls.

The first button remains the main mode button.

Buttons:

1. Value: loop length in bars
2. Value: playback speed in BPM
3. Value: CC output rate in Hz
4. Value: MIDI output channel

The MIDI output channel is global and applies to generated MIDI notes and CC messages.

## 20.1 Loop length

Changing loop length must not erase stored notes or CC points.

If the loop is shortened:

- data outside the new loop duration is ignored;
- it is not deleted.

If the loop is later extended:

- previously stored data becomes available again.

## 20.2 BPM

BPM changes playback speed immediately when not recording.

Recorded musical positions do not change.

## 20.3 CC output rate

Changes the output frequency of all four CC curves.

Default:

```text
50 Hz
```

## 20.4 MIDI output channel

Selects the MIDI channel used for all generated MIDI output.

Range:

```text
1..16
```

---

# 21. Initialization / default state

At boot:

```text
loop length        = 8 bars
BPM                = 120
CC output rate     = 50 Hz
quantization       = OFF
transport          = STOPPED
recording mode     = PLAY
selected CC curve  = 1
CC curve 1 number  = 1
CC curve 2 number  = 2
CC curve 3 number  = 3
CC curve 4 number  = 4
all CC curves      = MUTED
MIDI output channel = 1
```

The loop initially contains:

```text
no notes
no CC points
```

If a specific default is later changed, it should be changed here and not duplicated in unrelated code.

---

# 22. Important behavioral edge cases

The implementation must explicitly handle the following.

## Transport

- Stopping the loop while notes are active sends Note Off for all active notes.
- Stopping always resets the playhead to zero.
- Recording state returns to PLAY when transport stops.

## Notes

- Two stored notes cannot have the same pitch.
- Same-pitch overlapping notes are merged.
- Merging uses earliest start and latest end.
- If recording stops while a note is held, the recording stop position becomes that note's end.
- No note remains active across a loop boundary.
- Note velocity must remain in the MIDI range 0..127.

## Loop length

- Shortening the loop does not delete data.
- Data outside the current loop is ignored.
- Extending the loop makes previously ignored data available again.

## BPM

- BPM changes are not allowed while recording.
- BPM changes immediately affect playback speed.
- Musical positions do not change when BPM changes.

## CC

- Zero points → no CC output.
- One point → constant CC value over the entire loop.
- Multiple points → linear interpolation.
- CC values are clamped to 0..127.
- Muting a running curve immediately stops output for that curve.
- Unmuting resumes output.
- Clearing a curve removes all its points and therefore stops its output.

## UI

- If there are zero notes, no note is selected.
- Note-editing controls have no effect without a selected note.
- Delete has no effect without a selected note.
- Encoder values clamp at their valid minimum/maximum.
- Only one UI button is selected at a time.
- Rotation while editing a Label has no effect.

---

# 23. Software architecture

The implementation should be divided into logical modules.

A possible structure is:

```text
Core model
    Loop
    Note
    CCCurve
    CCPoint
    Settings

Transport / timing
    Transport
    MusicalClock
    Playhead
    Loop boundary handling

MIDI engine
    MIDI input
    Note event processing
    CC input processing
    Note scheduler
    CC scheduler
    Active-note tracking

Recording
    Note recording
    Note overdub/merge
    CC recording
    CC point management

UI
    UI framework
    Buttons
    Navigation
    Looper page
    Note Edit page
    CC Edit page
    Settings page

Hardware
    Run/Stop button
    Rotary encoder
    Display
    UART/debug
```

The exact file and class organization is an implementation choice.

## 23.1 Task architecture

Use FreeRTOS tasks.

The critical requirement is:

> MIDI event scheduling must have deterministic timing and must not be blocked by display rendering.

A possible architecture is:

```text
Core 0
    high-priority MIDI/timing task

Core 1
    lower-priority UI task
```

This is a recommended implementation approach, not a behavioral requirement.

The UI should target approximately 20 FPS but must never be allowed to compromise MIDI scheduling.

The implementation must also account for the ESP32 task watchdog (TWDT). Do not simply disable watchdog protection without understanding the consequences; manage task priority/blocking behavior appropriately.

## 23.2 Shared data

The loop model is shared between:

- MIDI/recording processing;
- UI.

Access must be synchronized so that the UI cannot observe partially modified notes or CC points.

The exact synchronization mechanism is an implementation detail.

The timing-critical MIDI path should avoid waiting on long UI operations.

---

# 24. Debugging

Debug output is available through UART.

Configuration:

```text
115200 baud
```

Debugging information should be useful for:

- transport state changes;
- recording state changes;
- MIDI input events;
- note recording/merge behavior;
- loop boundary events;
- CC recording;
- unexpected state transitions;
- timing/debug information when diagnosing scheduling problems.

Debug logging must not introduce significant timing disruption in the MIDI scheduling path.

---

# 25. Acceptance tests

These tests describe observable behavior that the implementation should satisfy.

## 25.1 Boot

1. Power on the looper.
2. Verify transport is STOPPED.
3. Verify recording mode is PLAY.
4. Verify loop length is 8 bars.
5. Verify BPM is 120.
6. Verify CC output rate is 50 Hz.
7. Verify all four CC curves are muted.
8. Verify no notes or CC points exist.

## 25.2 Basic playback

1. Start the loop with the Run/Stop button.
2. Verify the playhead starts at zero.
3. Verify the playhead advances continuously.
4. Verify it wraps to zero after one loop.
5. Stop the loop.
6. Verify the playhead returns to zero.

## 25.3 Note recording while stopped

1. Start with transport STOPPED and recording mode PLAY.
2. Select Record.
3. Verify state becomes ARMED.
4. Send a MIDI Note On.
5. Verify the loop starts automatically.
6. Verify recording state becomes RECORD.
7. Verify the input note is not immediately echoed.
8. Verify the note is played on the next loop iteration.
9. Stop recording.
10. Verify the note remains in the loop.

## 25.4 Note overdubbing

1. Create an existing note from position 1.0 to 2.0.
2. Start recording while running.
3. Record the same pitch from 0.5 to 1.5.
4. Verify the result is one note from 0.5 to 2.0.

## 25.5 Multiple-note merge

1. Create two notes of the same pitch:
   - 1.0 to 2.0
   - 3.0 to 4.0
2. Record a note of the same pitch from 1.5 to 3.5.
3. Verify the resulting data contains one note:
   - 1.0 to 4.0.

## 25.6 Loop boundary

1. Create a note close to the end of the loop.
2. Start playback.
3. Verify the note receives Note Off at the loop boundary.
4. Verify no MIDI note remains active into the next loop iteration.

## 25.7 BPM change

1. Create a note at a known musical position.
2. Play the loop at 120 BPM.
3. Change BPM to 60 while not recording.
4. Verify the same musical note position is reached at half the playback speed.
5. Verify the stored note position did not change.

## 25.8 BPM change during recording

1. Enter RECORD.
2. Attempt to change BPM.
3. Verify the BPM control cannot be modified while recording.

## 25.9 Loop shortening

1. Create a note near bar 8.
2. Set loop length to 4 bars.
3. Verify the note is not played.
4. Set loop length back to 8 bars.
5. Verify the note is available again.

## 25.10 Empty CC curve

1. Select a CC curve.
2. Clear the curve.
3. Start playback.
4. Verify no CC messages are generated for that curve.

## 25.11 Single CC point

1. Create one CC point with value 64.
2. Start playback.
3. Verify the curve outputs value 64 throughout the loop.

## 25.12 Multiple CC points

1. Create points:
   - 0.0 → 0
   - 1.0 → 127
2. Start playback.
3. Verify values between the points are linearly interpolated.

## 25.13 CC mute

1. Start playback with a populated CC curve.
2. Mute the curve.
3. Verify CC messages stop.
4. Unmute the curve.
5. Verify output resumes.

## 25.14 CC recording in LOCAL mode

1. Stop the loop.
2. Select CC recording.
3. Select LOCAL.
4. Rotate the encoder.
5. Verify the loop starts.
6. Verify CC points are recorded at the current musical positions.
7. Complete one loop.
8. Verify recording automatically returns to PLAY.

## 25.15 CC recording in MIDI IN mode

1. Stop the loop.
2. Select CC recording.
3. Select MIDI IN.
4. Send the selected CC.
5. Verify the loop starts.
6. Verify CC values are recorded at the corresponding musical positions.
7. Verify the input is not separately echoed as live MIDI output.
8. Complete one loop.
9. Verify recording returns to PLAY.

## 25.16 UI navigation

1. Start on Looper.
2. Rotate encoder.
3. Verify selection cycles through available buttons.
4. Click the encoder on a Value.
5. Verify edit mode starts.
6. Rotate encoder.
7. Verify value changes.
8. Click again.
9. Verify edit mode ends.
10. Navigate through all four pages.
11. Verify Settings wraps back to Looper.

## 25.17 Empty note editor

1. Ensure there are no notes.
2. Enter Note Edit.
3. Verify no note is selected.
4. Rotate/edit note controls.
5. Verify no invalid note is created.
6. Click Delete.
7. Verify nothing happens.

---

# 26. Development strategy

The project should be developed incrementally.

Recommended order:

```text
1. Project / PlatformIO setup
2. USB MIDI input/output
3. Musical clock and transport
4. Core note model
5. Note playback scheduler
6. Note recording
7. Note overdub/merge
8. CC curve model
9. CC playback
10. CC recording
11. UI framework
12. Looper page
13. Note Edit page
14. CC Edit page
15. Settings page
16. Integration
17. Acceptance tests
18. Timing/performance optimization
```

The coding agent should avoid implementing the entire project as one monolithic step.

Each stage should compile and, where practical, be testable before moving to the next stage.

---

# 27. Implementation freedom

The following are behavioral requirements and must be preserved:

- musical timing is based on 4/4 bars;
- musical positions remain stable when BPM changes;
- notes and CC points use musical positions rather than absolute milliseconds;
- notes are merged by pitch during overdubbing;
- no MIDI note remains active across a loop boundary;
- CC curves use points and interpolation;
- zero-point CC curves produce no output;
- one-point curves hold a constant value;
- MIDI scheduling is independent of display rendering;
- UI behavior follows the defined state and button rules.

The following may be decided by the implementation:

- exact C++ class names;
- exact source-file organization;
- exact container types;
- exact fixed-point timing resolution;
- exact FreeRTOS synchronization primitives;
- exact display rendering algorithms;
- exact MIDI scheduling data structures;
- exact UI component class hierarchy.

Implementation decisions should favor:

- deterministic timing;
- low memory usage;
- predictable execution time;
- simple ownership of data;
- testability;
- maintainability;
- minimal unnecessary abstraction.

---

# 28. Final implementation principle

The goal is not to make the implementation unnecessarily complex.

The goal is to make the behavioral model explicit enough that the implementation does not need to guess what the looper is supposed to do.

When a design decision affects observable musical behavior, it belongs in this specification.

When a design decision only affects how the code achieves that behavior, it should generally be left to the implementation.
