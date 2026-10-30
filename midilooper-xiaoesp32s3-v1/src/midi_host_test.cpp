// =============================================================================
//  midi_host_test.cpp - ESP32-S3 as USB-MIDI HOST, note reception logging
//
//  The XIAO ESP32-S3 enumerates a USB-MIDI device (the OP-1 field, or any
//  MIDI keyboard) on its native OTG port and logs every received MIDI event
//  packet on Serial1, same style as test_midi.cpp (device mode):
//    RX i=<itf> d=<us since prev> q=<queued> hdr=%02X [%02X %02X %02X]
//  - d is the microsecond delta between successive read batches (esp_timer).
//  - compact [stat] heartbeat every 5 s; mount/unmount events logged with
//    timestamps, captured in the TUH worker task and printed from loop().
//
//  Hardware notes (Seeed XIAO ESP32-S3):
//  - Attach the MIDI device through a USB-C OTG adapter on the XIAO port.
//  - The attached device needs 5V VBUS: power the XIAO from LiPo/battery or
//    the usual USB power in host mode is already on VBUS for this board.
//  - No pull-up wiring needed: the DWC2 controller drives the D+/D- lines.
//
//  Debug output: Serial1, D5/D6 @ 921600 (8N1).
// =============================================================================
#include <Arduino.h>
#include "USBHost.h"
#include "class/midi/midi_host.h"
#include "esp_timer.h"
// Force LDF to build lib/tinyusb-instrumented: it supplies the host-env
// usbh.c (CFG_TUH_ENUMERATION_BUFSIZE=1024) overriding the archive member.
#include <midi_diag.h>

#define TEST_BAUD     921600
#define TEST_RX_PIN   D5
#define TEST_TX_PIN   D6
#define STAT_MS       5000

USBHostClass &usbHost = USBHost;  // core-provided global host singleton

static int64_t lastRxUs[CFG_TUH_MIDI] = {0};  // per-interface last packet time
static bool     everMounted = false;
static int64_t  mountAtUs = 0;

// --- mount/unmount events, captured in TUH worker, printed in loop() --------
static volatile bool evMounted = false;
static volatile bool evUmounted = false;
static volatile uint8_t evDaddr = 0, evRxCables = 0, evTxCables = 0;

// Any-device detection (USB or not a MIDI device: fired for every attach,
// with VID/PID from the device descriptor). tuh_mount_cb is claimed by the
// core's USBHost wrapper, so the descriptor callback is the safe hook.
static volatile bool evDevAttached = false;
static volatile uint16_t evVid = 0, evPid = 0;

extern "C" void tuh_enum_descriptor_device_cb(uint8_t daddr, const tusb_desc_device_t *desc) {
  evVid = desc->idVendor;
  evPid = desc->idProduct;
  evDaddr = daddr;
  evDevAttached = true;
}

extern "C" void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *m) {
  evMounted = true;
  evDaddr = m->daddr;
  evRxCables = m->rx_cable_count;
  evTxCables = m->tx_cable_count;
}

extern "C" void tuh_midi_umount_cb(uint8_t idx) {
  evUmounted = true;
}

extern "C" void tuh_midi_descriptor_cb(uint8_t idx, const tuh_midi_descriptor_cb_t *d) {
  (void) idx;
  (void) d;
}

void setup() {
  Serial1.begin(TEST_BAUD, SERIAL_8N1, TEST_RX_PIN, TEST_TX_PIN);
  Serial1.println("\n[midi_host_test] USB MIDI HOST test booting...");
  Serial1.printf("[midi_host_test] CFG_TUH_MIDI=%u\n", (unsigned)CFG_TUH_MIDI);

  if (!usbHost.begin()) {
    Serial1.println("[midi_host_test] USB host init FAILED");
    while (1) delay(1000);
  }

  Serial1.println("[midi_host_test] ready (921600). Connect a USB-MIDI device "
                  "via OTG adapter; it needs 5V VBUS.");
}

void loop() {
  const int64_t nowUs = esp_timer_get_time();

  // --- mount/unmount events, printed from this task -----------------------
  if (evDevAttached) {
    evDevAttached = false;
    Serial1.printf("USB device attached t=%.3fs daddr=%u vid=%04X pid=%04X\r\n",
                   nowUs / 1e6, evDaddr, evVid, evPid);
  }
  if (evMounted) {
    evMounted = false;
    everMounted = true;
    mountAtUs = nowUs;
    Serial1.printf("MIDI device mounted t=%.3fs daddr=%u rx_cables=%u "
                   "tx_cables=%u\r\n", nowUs / 1e6, evDaddr,
                   evRxCables, evTxCables);
  }
  if (evUmounted) {
    evUmounted = false;
    Serial1.printf("MIDI device UNMOUNTED t=%.3fs (was up %.3fs)\r\n",
                   nowUs / 1e6, (nowUs - mountAtUs) / 1e6);
  }

  // --- read every MIDI interface of the attached device -------------------
  for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++) {
    if (!tuh_midi_mounted(idx)) continue;
    const uint32_t avail = tuh_midi_read_available(idx);
    if (avail == 0) continue;

    uint8_t batch[4*16];
    uint32_t nb = tuh_midi_packet_read_n(idx, batch, 16);
    if (nb == 0) continue;

    for (uint32_t i = 0; i < nb; i++) {
      const uint8_t *p = &batch[4*i];
      const uint64_t prev = lastRxUs[idx];
      const uint64_t delta = (prev == 0) ? 0 : nowUs - prev;
      lastRxUs[idx] = nowUs;
      const char *tag = (prev == 0) ? "(first)"
                        : (delta > 2000000) ? "(>2ms!)"
                        : (delta > 1000) ? "(batch)" : "";
      Serial1.printf("RX i=%u d=%-9llu q=%-3u hdr=%02X [%02X %02X %02X] %s\r\n",
                     idx, delta, avail, p[0], p[1], p[2], p[3], tag);
    }
  }

  // --- heartbeat -----------------------------------------------------------
  static int64_t lastStatUs = 0;
  if (nowUs - lastStatUs >= STAT_MS * 1000LL) {
    for (uint8_t idx = 0; idx < CFG_TUH_MIDI; idx++) {
      Serial1.printf("[stat] t=%llds itf%u:%s rx_all=%s usbh=%s\r\n",
                     nowUs / 1000000, idx,
                     tuh_midi_mounted(idx) ? "MOUNTED" : "-",
                     tuh_midi_read_available(idx) ? "PENDING" : "drained",
                     tuh_inited() ? "host-running" : "host-off");
    }
    lastStatUs = nowUs;
  }
}
