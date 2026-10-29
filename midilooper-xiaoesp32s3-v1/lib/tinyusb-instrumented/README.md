# TinyUSB MIDI RX instrumentation (`lib/tinyusb-instrumented`)

Diagnostic rebuild of the TinyUSB device stack used by the Arduino core, to
localize which stage withholds the packets of a multi-event USB-MIDI OUT
burst: the DWC2 USB controller (DCD), the TinyUSB device stack (USBD), or the
MIDI class driver.

## What this replaces

The Arduino core ships TinyUSB as the **prebuilt archive**
`framework-arduinoespressif32-libs/esp32s3/lib/libarduino_tinyusb.a` (only
headers are present on disk). Its provenance (recorded in
`esp32s3/versions.txt`):

| Component | Version |
|---|---|
| arduino core | `espressif/arduino-esp32@e7c1e8cd4` (3.3.x) |
| TinyUSB | `espressif/tinyusb@88c7f0b81` (master, ~TinyUSB 0.21.x line) |
| lib-builder | `espressif/esp32-arduino-lib-builder@6671d0b`, per-target `-DCFG_TUSB_MCU=OPT_MCU_ESP32S3` |

`src/` here contains instrumented copies of the four device-stack sources that
matter for MIDI RX:

| File (compiled unit) | Stock archive member replaced |
|---|---|
| `src/dcd_dwc2.c` | `dcd_dwc2.c.obj` (DWC2 DCD) |
| `src/usbd.c` | `usbd.c.obj` (device stack) |
| `src/tusb.c` | `tusb.c.obj` (`tu_edpt_stream_*` arm/read path) |
| `src/midi_device.c` | `midi_device.c.obj` (MIDI class driver) |
| `src/midi_diag.c` | (new: shared event ring buffer) |

`reference/` holds pristine downloaded copies for diffing.

Because the instrumented objects define the same symbols and are linked before
the archive, GNU ld never pulls the corresponding stock members
(verify with `firmware.map`: after linking, `usbd.c.obj`/`midi_device.c.obj`/`tusb.c.obj`/`dcd_dwc2.c.obj`
must appear **outside** `libarduino_tinyusb.a(...)`).

## Enabling / disabling

`env:midi_test` in `platformio.ini` sets `-DMIDI_DIAG=1`. Remove or comment
that line to inert-out every hook (`midi_diag.h` no-ops at compile time
without usage cost): the objects still rebuild, but no buffering, timestamps,
or state reads happen inside the hooks; behavior is stock. The main firmware
env (`seeed_xiao_esp32s3`) does not reference this library, so it always links
the stock archive untouched.

## Reproducing the issue and interpreting the log

1. Flash `env:midi_test`; open **Serial1 (D5/D6 @ 115200)** (USB CDC is not
   available - the sketch runs as a TinyUSB MIDI device).
2. On the host, send to the device a multi-event burst in one transfer
   (e.g. press a 5-note chord at once through MIDI-OX/routeMIDI/DAW), hold,
   release, then send a single event, and note the `[diag]` event stream for
   the bursts.
3. Read the RX chain bottom-up in output:

- `DIAG_DCD_RXPKT` in the **same IRQ pass** for every MIDI event packet of the
  burst => the controller delivered all packets: culprit is *above* the DCD.
  DCD events: `DCD_ARM` = OUT endpoint armed by the app/class re-arm
  (`aux=ret`); `DCD_DONE` = OUT XFERCOMPL event queued to the device stack.
- `DIAG_USBD_DISP` (task context, `usbd.c` `DCD_EVENT_XFER_COMPLETE`) and
  `USBD_ISRQ` (dcd_event_handler inline / deferred `xfer_isr`) tell whether the
  device stack received each packet's completion event promptly, and
  `DIAG_MIDI_CB`/`DIAG_MIDI_RX`/`DIAG_MIDI_ARM` tell whether the MIDI class
  pushed it into the RX stream FIFO and re-armed the endpoint.
- `DIAG_MIDI_ARM` `len=0` => the class could not re-arm the OUT endpoint at
  that time (blocked). Cross-check the neighbor lines:
  - `STRM_BLK` `len=mps aux=remaining`: `tu_fifo_remaining() < mps` (0.21
    requires the whole FIFO free).
  - `STRM_CLAIM` `aux=remaining`: `tu_edpt_claim()` failed (e.g. the
    `RX_PENDING`/ownership hold or BUSY state).
  - `STRM_SHRINK` `len=remaining`: claim lost the space after re-get.
  - `STRM_ARM` `len=count>0`: re-arm succeeded.
- `DIAG_DROP` would indicate dropped device events (event queue full) - not
  expected here.

Event timestamps are `esp_timer_get_time()` microseconds, captured in the ring
(`midi_diag.c`, no printing from ISR) and printed by `test_midi.cpp` from the
application loop, prefixed `+delta_us`.

## Files

- `include/midi_diag.h` - event types + ring API (hook macros no-op without `MIDI_DIAG`)
- `src/midi_diag.c` - ring buffer implementation (task + ISR safe, single reader)
- `src/dcd_dwc2.c` - RX packet / XFERCOMPL / OUT arm logging
- `src/usbd.c` - dcd_event_handler, xfer_cb dispatch, queue-full drop logging
- `src/tusb.c` - `tu_edpt_stream_read_xfer()` blocked/claim/re-arm logging
- `src/midi_device.c` - `midid_xfer_cb` ep/bytes/FIFO state + re-arm result
- `reference/` - pristine TinyUSB `@88c7f0b81` sources for diffing
