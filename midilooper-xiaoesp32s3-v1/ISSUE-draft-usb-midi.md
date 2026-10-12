# Draft GitHub issue — arduino-esp32 (espressif/arduino-esp32)

> Consider cross-filing at `hathach/tinyusb` if the root cause turns out to be
> in the stack rather than the core integration.

---

**Title:** USB-MIDI device: only the first event packet of a multi-event OUT burst is delivered; the rest are withheld until the *next* transfer arrives

## Environment

- **Board:** Seeed XIAO ESP32-S3 (ESP32-S3, USB-OTG)
- **Platforms (identical symptom on both):**
  - pioarduino `platform-espressif32` **stable** (arduino-esp32 3.3.x fork)
  - pioarduino `platform-espressif32` **61.04.00-RC1** (IDF 6.1 RC; bundled **TinyUSB 0.21.0**, per `TUSB_VERSION_*`)
- `build_flags = -DARDUINO_USB_MODE=0` (TinyUSB device mode)
- Repro uses only the core `USBMIDI` class — no MIDI library, no app logic,
  no extra tasks, no ISRs.

## Symptom

When the host sends several MIDI events in one bus burst (e.g. the Note Ons of
a chord, which Windows packs into a single OUT transfer), the device delivers
**only the first 4-byte event packet** immediately. The remaining packets are
held and only delivered **with the first packet of the *next* transfer**,
whenever that occurs — for a chord, at key release (seconds later).

Events are not lost, just grossly delayed and batched.

## Minimal repro sketch

Debug output on Serial1 (D5/D6 @ 115200; USB CDC is replaced by the MIDI device).

```cpp
#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>

USBMIDI midi;

void setup() {
  Serial1.begin(115200, SERIAL_8N1, D5, D6);
  midi.begin();
  USB.begin();
  Serial1.println("[test_midi] ready");
}

void loop() {
  static int64_t lastRxUs = 0;

  uint32_t avail = tud_midi_available();        // bytes in the RX FIFO right now
  uint8_t batch[4 * 16];
  uint32_t n = tud_midi_packet_read_n(batch, 16);
  if (n > 0) {
    int64_t nowUs = esp_timer_get_time();
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *p = &batch[4 * i];
      int64_t d = lastRxUs == 0 ? 0 : nowUs - lastRxUs;
      Serial1.printf("RX d=%-9lld q=%-3u hdr=%02X [%02X %02X %02X]%s\r\n",
                     d, (unsigned)avail, p[0], p[1], p[2], p[3],
                     (i == 0 && n > 1) ? " (batch)" : "");
      lastRxUs = nowUs;
    }
  }
}
```

## Steps to reproduce

1. Flash, open Serial1 at 115200.
2. Send MIDI from the host to the ESP32 device (e.g. a USB-MIDI keyboard
   routed through the host, MIDI-OX, or a DAW).
3. Press two keys simultaneously; hold ~1 s; release both; repeat.
4. Watch the Serial1 stamps (`d = µs since previous packet`).

## Observed (stable platform; 5-key chord 0x29 2B 2D 2F 30, then tap 0x23)

Human pattern: press 5-key chord → ~125 ms later release it → ~1 s later tap
(note 0x23) → ~125 ms later release it. One full cycle of the log; the pattern
repeats identically over multiple cycles:

```text
USB mounted (enumerated) t=0.298s
RX d=4029925   q=4   hdr=09 [90 29 7F]                       <- chord ONs: ONLY 1st packet of burst B1
RX d=126009    q=20  hdr=09 [90 2B 7F] (batch)               <- at release (B2): withheld
RX d=0         q=20  hdr=09 [90 2D 7F]                       <-    2B/2D/2F/30 ONs arrive together
RX d=0         q=20  hdr=09 [90 2F 7F]                       <-    with the 1st packet of B2 (29 OFF)
RX d=0         q=20  hdr=09 [90 30 7F]
RX d=0         q=20  hdr=08 [80 29 00]
RX d=874987    q=0   hdr=08 [80 2B 00] (batch)               <- at tap (B3): withheld
RX d=0         q=0   hdr=08 [80 2D 00]                       <-    2B/2D/2F/30 OFFs arrive together
RX d=0         q=0   hdr=08 [80 2F 00]                       <-    with the 1st packet of B3 (23 ON)
RX d=0         q=0   hdr=08 [80 30 00]
RX d=0         q=0   hdr=09 [90 23 7F]
RX d=124987    q=4   hdr=08 [80 23 00] (batch)               <- B4 (23 OFF) has nothing pending:
                                                               delivered immediately
```

Single-event transfers (e.g. the host's USB-MIDI identity requests
`F0 7E 7F` / `06 01 F7`) always arrive whole and immediately.

### Interpretation

| Host burst (one transfer) | Delivered immediately | Delivered later |
|---|---|---|
| B1: 5x Note On | `29ON` only | `2B/2D/2F/30 ON` |
| B2: 5x Note Off | `29OFF` only | `2B/2D/2F/30 OFF` |
| B3: `23ON` (first packet) | – | flushed together with B3 |
| B4: `23OFF` (nothing pending) | `23OFF` | – |

Key observations:

- **`q=4` on burst B1** means only ~one event packet was in the RX FIFO when
  the first became visible — the withheld packets are **not** queued in the
  FIFO; they sit below it, in the device stack.
- The poll loop runs at ~10^5/s, so FIFO-pop latency is not a factor.
- Leftovers always flush **in FIFO order with the first packet of the next
  burst**, whatever that burst is (a single Note On suffices).
- Same byte-exact signature on stable **and** 61.04.00-RC1 / TinyUSB 0.21.0 →
  not specific to one core version.

## Analysis

RX chain for `tud_midi_n_packet_read()` (sources at tag 0.21.0):

- `src/class/midi/midi_device.c`: `tud_midi_n_packet_read()` →
  `tu_edpt_stream_read()` (pop FIFO) + `tu_edpt_stream_read_xfer()` (re-arm).
- `src/tusb.c`: `tu_edpt_stream_read_xfer()` arms the OUT endpoint only if
  `tu_fifo_remaining() >= mps` **and** `tu_edpt_claim()` succeeds. In 0.21.0
  `tu_edpt_claim()` refuses while `TU_EDPT_STATE_RX_PENDING` is set (the
  #1292 "OUT xfer completed, buffer not yet consumed by the class" hold),
  cleared only by `usbd_edpt_rx_consume()` from `midid_xfer_cb`.
- `midid_xfer_cb` runs in the `usbd` task; data only reaches the FIFO from
  there.

The one-packet-per-burst delivery suggests that after the first completed
transaction of a multi-transaction burst, the remaining transactions' data is
not pushed to the FIFO and re-arming is blocked, until the next host transfer
triggers a completion event that finally lets the withheld packets through.
The application cannot work around this: at that point
`tud_midi_packet_read()` pops nothing (FIFO empty) and
`tu_edpt_stream_read_xfer()` cannot claim/re-arm the endpoint.

Hypotheses (for maintainers to confirm/refute):

1. With a 64-byte armed OUT transfer, each 4-byte USB-MIDI event packet is a
   short packet; if `dcd_dwc2` signals XFERCOMPL only for the transaction that
   ends the armed count (or the class processes only one `xferred_bytes`
   chunk), subsequent same-frame packets sit in the DWC2 RX path until the
   next armed transfer completes — matching the "flushed by next burst"
   behavior.
2. The 0.21.0 `RX_PENDING` hold (#1292) combined with a one-transaction
   `xfer_cb` could leave the endpoint unclaimable, deferring everything until
   the next transfer's `xfer_cb` runs.

## Expected

All event packets of a received multi-event burst should be pushed to the RX
FIFO on arrival (same USB frame = ~1 ms), so `tud_midi_available()` /
`tud_midi_packet_read*()` return them immediately, like other USB-MIDI device
implementations.

## Notes

- Both platforms verified with the same sketch, same keyboard, same host.
- Happy to test patches against these snapshots or a newer TinyUSB.
