# ESP32-MIDILOOP — description v3 (midilooper-xiaoesp32s3-v1)

This is a small USB MIDI looper build around a Xiao ESP32S3 on Arduino: a pure monochrome LCD display, a clickable rotary encoder and a main Run/Stop button. The instrument (e.g. Teenage Engineering OP-1 field) connects as a **USB device** to the looper, which runs the **USB host** role on its single OTG port.

This document describes the **current, implemented** behavior of `midilooper-xiaoesp32s3-v1`. It supersedes `description-v2.md`, which still described the earlier device-mode design (Control Surface / USB device role). Where implementation details are not specified here, the coding agent may choose a suitable implementation, but must not invent new user-visible behavior without documenting it here.

## Summary

- The looper records MIDI notes from the instrument into a loop of whole bars, plays them back repeatedly and supports overdubbing. Notes captured while recording are **not echoed** back; they play at the next loop pass.
- It supports 4 MIDI CC automation curves, recorded live (MIDI IN) or drawn with the rotary encoder (LOCAL), editable and muteable independently, output on the USB MIDI out at a configurable rate with interpolated values.
- The physical main button (D4) starts/stops the transport.
- Recording supports an **armed** workflow: ARM (wait for first input, record continuously), ARM1 (wait for first input, record exactly one loop cycle, then auto-return to play), operated from the Looper and CC Edit pages.
- Note and CC positions are internally stored as fixed-point ticks (1000 ticks per beat); the fractional-bars representation of v2 still applies conceptually.
- Note editing (pitch/start/duration/velocity/new/delete/preview) with a selectable **quantization grid** (OFF, 1, 1/2, 1/4, 1/8 of a beat).
- Loop length 1–64 bars, BPM 20–300 adjustable in real time while playing (locked while recording), MIDI out channel 1–16, CC output rate 1–200 Hz (default 30 Hz), CC knob sensitivity for LOCAL curve drawing.
- Incoming MIDI messages all channels are accepted (the input channel is ignored); output uses the single configured MIDI out channel.
- No live passthrough of MIDI input: incoming notes are only recorded (and displayed on the indicators/monitor); there is deliberately no note echo while the transport runs.

## Software architecture

FreeRTOS tasks (see [main.cpp](midilooper-xiaoesp32s3-v1/src/main.cpp)):

- **MIDI task** ([midi_engine.cpp](midilooper-xiaoesp32s3-v1/src/midi_engine.cpp)): pinned to core 0, high priority. Owns the TinyUSB host stack, pumps and parses USB-MIDI event packets, records notes/CC into the shared model, schedules note/CC playback and note previews. Core 0 idle watchdog is disabled (`disableCore0WDT()`). The task yields (`taskYIELD()` + 1 ms delay) each iteration to keep core 0 from saturating, which otherwise worsens rare DWC2 interrupt-latency races. It must never block long on the model mutex (non-blocking take `0`, skip on failure, except playback scheduling which takes a short 4 ms budget).
- **Display/UI render task** ([display.cpp](midilooper-xiaoesp32s3-v1/src/display.cpp)): core 1, low priority, renders complete frames into the Adafruit_SharpMem buffer then refreshes. Snapshot the model under the mutex when the frame needs it. Never uses `clearDisplay()` (would flash white); only `clearDisplayBuffer()` + `refresh()`.
- **Main loop** ([main.cpp](midilooper-xiaoesp32s3-v1/src/main.cpp)): reads inputs ([input.cpp](midilooper-xiaoesp32s3-v1/src/input.cpp)), feeds `UI::update(encoderDelta, clicked)` (~200 Hz poll) and drives the recording LED (on while any recording is active).

Module layout under [src/](midilooper-xiaoesp32s3-v1/src):

| File | Role |
|---|---|
| [config.h](midilooper-xiaoesp32s3-v1/include/config.h) | Pin map, capacities, musical resolution, display geometry, debug helpers |
| [model.h](midilooper-xiaoesp32s3-v1/include/model.h) / [model.cpp](midilooper-xiaoesp32s3-v1/src/model.cpp) | Notes, CC curves, settings; mutex-guarded shared state; merge/delete/navigation helpers; MIDI input log |
| [transport.h](midilooper-xiaoesp32s3-v1/include/transport.h) / [transport.cpp](midilooper-xiaoesp32s3-v1/src/transport.cpp) | Transport state machine, musical clock (tick <- us conversion), record flags |
| [midi_engine.h](midilooper-xiaoesp32s3-v1/include/midi_engine.h) / [midi_engine.cpp](midilooper-xiaoesp32s3-v1/src/midi_engine.cpp) | USB-MIDI host I/O, RX dispatch, recording capture, playback scheduling, preview |
| [ui.h](midilooper-xiaoesp32s3-v1/include/ui.h) / [ui.cpp](midilooper-xiaoesp32s3-v1/src/ui.cpp) | Page/field tables, focus/edit navigation, blink feedback |
| [display.cpp](midilooper-xiaoesp32s3-v1/src/display.cpp) | Frame renderer: buttons, charts, icons, grids, monitor |

Adding UI fields/buttons is done by appending a row to a page's static `Field` table — navigation and rendering code must not need changes.

Build: PlatformIO, env `seeed_xiao_esp32s3` ([platformio.ini](midilooper-xiaoesp32s3-v1/platformio.ini)), pioarduino fork of `platform-espressif32` (Arduino core 3.x with TinyUSB). USB host flags (`ARDUINO_USB_MODE=0`, `ARDUINO_USB_ON_BOOT=0`, `ARDUINO_USB_CDC_ON_BOOT=0`, `ARDUINO_HOST_ENV=1`). `ARDUINO_HOST_ENV=1` compiles [usbh.c](midilooper-xiaoesp32s3-v1/lib/usb-host-enumeration-fix/src/usbh.c) with `CFG_TUH_ENUMERATION_BUFSIZE=1024` because composite audio+MIDI devices (OP-1 field) exceed the 256-byte descriptor buffer the stock prebuilt TinyUSB archive was compiled with. Compilation errors must be checked after changes. Debug goes to Serial1 at 115200 bauds (TX on D6 only wired).

## Project hardware

- Xiao ESP32S3, Arduino framework via PlatformIO.
- USB MIDI **host** on the single OTG port (TinyUSB). The library is **not** Control Surface anymore: its `USBMIDI_Interface` is a device-stack driver and was removed. Raw `tuh_midi_*` calls + a small CIN parser are used instead ([midi_engine.cpp](midilooper-xiaoesp32s3-v1/src/midi_engine.cpp)).
- Run/Stop button on D4.
- Rotary encoder A/B on D1/D2, knob press on D3.
- Debug UART: TX on D6, RX on D5 (only TX wired).
- Built-in LED on `LED_BUILTIN` (on while recording).
- Sharp memory LCD, Adafruit/Adafruit SHARP Memory Display + Adafruit_GFX: SCK D8, MOSI D10, SS D7. 400x240 pixels, ~58x35 mm, monochrome (black/white). High pixel density must be considered for UI readability.

## Musical timing model

- 4/4, `TICKS_PER_BEAT = 1000`, `TICKS_PER_BAR = 4000` fixed-point ([config.h](midilooper-xiaoesp32s3-v1/include/config.h)). Fractional-bar positions are derived on demand: `bars = tick / TICKS_PER_BAR`.
- 1 bar = 4 beats; BPM 120 => 1 beat = 0.5 s, 1 bar = 2 s. Loop length is whole bars (default 2, range 1–64).
- Playback position wraps at the loop boundary. BPM changes take effect immediately while playing (playback speed only; recorded positions never change). **BPM cannot be changed while a recording session is active** (`Transport::setBpm` refuses then). Loop length changes never erase notes or CC points; data beyond the new length is simply ignored.
- Changing the loop length requires **no playhead adjustment**: the tick rate depends only on BPM (`ticksFromUs` does not involve the loop length), so changing the bar count only changes the modulo wrap point.
- Stored note/CC positions are **never altered** by quantization: the quantization grid only affects the edit step and the playback/effective positions (`Model::quantizeTick` / `Model::effectiveStart/End`).

A note contains:
- `id` unique (conceptually increments with recorded notes)
- `pitch` 0..127
- `velocity` 0..127
- `startTick` musical start in ticks
- `durationTick` musical length in ticks; `endTick = startTick + durationTick`

If recording stops (or leaves RECORD) while a note is held, the note-off is finalized at the moment of the stop (or at the loop boundary for a one-shot ARMED_ONCE recording that ends there).

**Overdubbing / merging** ([model.cpp](midilooper-xiaoesp32s3-v1/src/model.cpp)): a recorded note whose pitch matches an existing note is merged into it — earliest start, latest end, single velocity taken from the newly recorded Note On, the pre-existing note's id preserved. Same rule applies when the Note Edit page changes a note's pitch into another note's pitch (that note's id and velocity are kept).

## USB MIDI host behavior

- Only one instrument is expected. Device identity (VID/PID, cable counts, uptime) is captured on mount for the Settings page status line.
- Incoming 4-byte USB-MIDI event packets are dispatched directly. The OP-1 field sends proper status bytes; packets carrying CIN < 0x8 (running-status data) are re-materialized against the remembered status byte. Single-data-byte messages (PC, channel pressure) never consume a garbage last data slot.
- Every incoming channel message is appended (non-blocking) to a ring-buffer MIDI log, shown on the Settings page.
- Notes: `handleNoteOn` (with velocity 0 treated as note off), `handleNoteOff`, CC via `handleCcIn`. First recordable input while ARMED/ARMED_ONCE and stopped auto-starts the transport and converts the armed state to RECORD.
- Output: `txNoteOn/txNoteOff/txControlChange` through the mounted interface on the configured channel. All note-offs are flushed at loop wrap and on transport stop, so no note survives a loop boundary or stop.
- Note preview (audition): when editing notes while the transport is stopped, each pitch/duration/velocity/creation change triggers a short (150 ms) note on/off through the MIDI engine.

## Common UI navigation

Buttons are stacked vertically on the left column (100 px = 25% of width); the chart area occupies the remaining width minus a 5 px right-hand MIDI input column. Top button is always the mode/page switch (icon only). Clicking it cycles pages: Looper -> Note Edit -> CC Edit -> Settings -> Looper. Icons: looping arrow, black quarter note, "CC" (text size 2), cog.

Field types ([ui.h](midilooper-xiaoesp32s3-v1/include/ui.h)):
- **VALUE**: click enters edit mode (button inverted), rotation edits the value, second click exits.
- **OPTION**: click enters edit mode, rotation cycles enumeration values, second click exits.
- **LABEL**: click performs an action; momentary labels blink (color inversion) 3 times (~600 ms) to acknowledge; some labels sustain an "activated" inverted look from their `isActive` callback (e.g. RUN/STOP, REC).
- **MAIN**: page switch, no activation.

When the selected button is not editing, rotation moves the focus (3 px outline vs 1 px outline when unfocused). Activation/editing/active/blink inverts the button colors.

Navigation is overridden on the CC Edit page while a **LOCAL CC recording session** is armed/recording: rotation feeds the curve value (`MidiEngine::applyLocalCcDelta`), clicking still works to cycle the record states.

## Looper mode page

Left buttons:
- MAIN (loop icon)
- LABEL (sustained): RUN/STOP status — toggled by the physical D4 button; click does nothing.
- LABEL: Play/Record — click cycles while stopped: PLAY -> ARM -> ARM1 -> PLAY; while running: PLAY <-> RECORD directly. Displays `REC` / `ARM` / `ARM1` / `PLAY`.
- OPTION: Quantization grid — `QUANT OFF / 1 / 1/2 / 1/4 / 1/8`; shared with the Note Edit page.
- VALUE: BPM.
- LABEL: `N BARS` (read-only; edited on Settings).
- LABEL (momentary): `CLEAR` — erases all notes of the loop (CC curves untouched).

Center chart: notes as filled horizontal rectangles (4 px tall) mapped over about 5 octaves (note 36..96) over the full height; dashed vertical bar lines (pattern 1,0 and 1,0,0 when quantized) and dashed beat lines (1,0,0,0,0 — shorter patterns while quantized). When quantized, subdivision markers finer than a beat are added. A grayed (chessboard dither) version of the enabled CC curves is drawn in the background, as on the CC Edit page. A playhead is implied by the edit/record timing but no dedicated head line is drawn.

Right column (5 px): left-pointing 4 px triangles, one per currently held incoming MIDI note (appear on Note On, disappear on Note Off).

## Note Edit page

Left buttons:
- MAIN (quarter-note icon)
- VALUE: ID — cycles through recorded notes in ascending id order; updates pitch/start/duration/velocity displays.
- OPTION: Note name (pitch), e.g. `C#4 (61)`.
- VALUE: START (fractional bars, 2 decimals).
- VALUE: DUR.
- VALUE: VEL.
- OPTION: Quantization grid (shared with Looper).
- LABEL (momentary): `NEW` — creates a note with pitch = last received Note On's pitch, start = selected note's end (0 if none), duration = one beat, selects it. Merges as per the overdub rule on pitch collision. Blinks only if the note was actually stored (id != 255).
- LABEL (momentary): `DELETE` — removes the selected note; selection moves to the previous id (or the first note).

`NEW` inheritance detail: the new note starts where the previously selected note ended; if no note is selected it starts at 0.

Editing behavior:
- While the transport is stopped, each pitch/duration/velocity/new-note edit triggers an audition preview of the edited note. Start position edits do not preview.
- Changing start/duration moves the rectangle in the chart; changing only velocity does not move anything.
- Edit step: quantized => one quantization step per encoder tick; non-quantized => one hundredth of a bar.

Center chart: same grid as the Looper; the selected note (by id) is surrounded by a dithered chessboard rounded rectangle, 3 px line width. When quantized, the drawn rectangle shows the **raw** stored position with thin extensions/notches indicating where the quantized playback start/end will actually fall (black extension = longer, white notch = shorter).

Right column: same incoming-note triangles as the Looper.

## CC Edit page

Left buttons:
- MAIN (CC icon)
- LABEL (sustained): RUN/STOP status (same transport).
- OPTION: `CURVE 1..4` — selects which curve is highlighted/edited.
- LABEL: Play/Record — same arm workflow as the Looper (drives `ccRecord`): PLAY -> ARM -> ARM1 -> PLAY stopped, PLAY <-> RECORD running.
- OPTION: `MIDI IN` / `LOCAL` — input source for the selected curve.
- VALUE: BPM.
- OPTION: `MUTE ON/OFF` — muted curves output nothing (default ON for all curves).
- VALUE: CC number 0..127 (default 1, 2, 3, 4 per curve).
- LABEL (momentary): `CLEAR` — erases the selected curve's points.

The bars label from description-v2 was removed (loop length is edited on Settings only).

Logic:
- The recording only affects the **selected** curve. MIDI IN recording captures only incoming CC messages whose number matches the curve; LOCAL recording uses the knob.
- Starting a LOCAL record session from PLAY (re)initializes the "follow the curve" baseline. The knob indicator follows the curve value until the encoder is first rotated; from then the curve is overridden: every point at/after the playhead is erased ahead and new points are written. While running, an ARMED LOCAL curve converts to RECORD on the first knob rotation (the rotation is dedicated to the curve, not to navigation). A freshly overridden value is transmitted immediately, not rate-limited.
- While recording in MIDI IN mode with the loop running, matching incoming CC values override the curve points under the playhead.
- Ending a LOCAL record session (`exitCcRecord`) returns to the normal navigation.

Center chart: selected curve 3 px solid black; other curves 3 px dithered (chessboard). Muted/empty curves are not drawn. Values are linearly interpolated between points.

Right column: one triangle at the selected curve's current value — last received CC value in MIDI IN mode, knob-driven value in LOCAL mode.

## Settings page

Buttons (full-width column is NOT used anymore; the layout stays the standard left column, with the right area showing a live incoming-MIDI monitor):
- MAIN (cog icon)
- VALUE: `BARS` (loop length)
- VALUE: `BPM`
- VALUE: `CC RATE` in Hz (1–200, default 30)
- VALUE: `MIDI CH` out channel 1–16
- VALUE: `CC KNOB SENS` (1–10, default 2) — multiplies each encoder step during LOCAL CC recording.

The right area is the **MIDI IN monitor**: a `MIDI IN` title plus `USB HOST` device status line (VID:PID, IN/OUT cable counts, connection uptime, or `no device`), a separator line, then the newest 64 incoming messages as scrolling text lines (newest at the bottom), each formatted `s.mmm TYPE CHnn <details>`: note on/off with note name and velocity, CC with number=value, or raw other messages with data bytes.

## Transport record state machine

`RecordState = PLAY | RECORD | ARMED | ARMED_ONCE` ([transport.h](midilooper-xiaoesp32s3-v1/include/transport.h)); independent flags for notes (`noteRecord`) and CC (`ccRecord`) while sharing one playhead/transport. `ARMED` records continuously after the trigger; `ARMED_ONCE` records exactly one loop cycle after the trigger, then auto-returns to PLAY (handled by the MIDI engine at loop wrap). Both auto-convert to RECORD on the triggering first input, or when the transport is started manually. Stopping the transport returns both flags to PLAY and finalizes/backfills the notes.

## Persistence and future work

No settings/pattern persistence is implemented yet (everything resets on boot). Possible future work: flash/NVS persistence, live MIDI passthrough option, loop-length-from-first-recording, per-curve CC channel.
