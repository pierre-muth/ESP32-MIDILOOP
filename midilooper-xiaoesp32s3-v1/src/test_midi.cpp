// =============================================================================
//  test_midi.cpp - MINIMAL USB MIDI RX test (standalone sketch), v2
//
//  Purpose: isolate the rare "incoming MIDI message only dispatched when the
//  next one arrives" issue inside the Arduino core / TinyUSB / DWC2 stack.
//  RX confirmed working; this version focuses on chord timing analysis
//  (two notes pressed together should share one USB frame).
//
//  Output format (Serial1, D5/D6 @ 115200):
//    RX d=<us since prev> hdr=%02X [%02X %02X %02X] <tag>
//  - one line per MIDI USB packet, timestamped in MICROSECONDS relative to
//    the previous packet (esp_timer). Packets of the same USB frame arrive
//    back-to-back (tens of us). A chord note showing a >1ms delta means the
//    previous packet was delayed in the driver -> the bug fingerprint.
//  - tag marks gaps (>1ms / >2ms since prev).
//  - compact [stat] heartbeat every 5 s; TX probe every 2 s (can be muted
//    with TX_PROBE_MS 0) so the host side can still verify connectivity.
// =============================================================================
#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>
#include <esp_timer.h>
#include <midi_diag.h>

#define TEST_BAUD     115200
#define TEST_RX_PIN   D5
#define TEST_TX_PIN   D6
#define MIDI_CH       1
#define TX_PROBE_MS   0   // TX probe period (0 disables the probe)
#define TX_HOLD_MS    300

USBMIDI midiRx;

static int64_t lastRxUs = 0;
static uint32_t rxCount = 0;
static uint32_t slowCount = 0;     // packets >1ms after predecessor
static int64_t maxDeltaUs = 0;    // largest observed inter-packet delta

// --- USB mount watching -----------------------------------------------------
// tud_mounted() flips true only when the host finished enumeration
// (SET_CONFIGURATION). We watch for transitions so a mount-then-drop is
// distinguishable from a never-mounted boot.
static bool lastMounted = false;
static bool everMounted = false;
static int64_t mountAtUs = 0;      // when it (first) mounted

// --- TX probe state ---------------------------------------------------------
static bool     txActive = false;
static uint8_t  txNote = 0;
static int64_t  txOnAtUs = 0;
static int64_t  nextTxAtUs = 0;
static uint32_t txCount = 0;

static void printPacket(int64_t deltaUs, const midiEventPacket_t &pkt) {
  char tag[24] = "";
  if (lastRxUs == 0)
    strcpy(tag, "(first)");
  else if (deltaUs > 2000)
    strcpy(tag, "(>2ms!)");
  else if (deltaUs > 1000)
    strcpy(tag, "(>1ms)");
  Serial1.printf("RX d=%-9lld hdr=%02X [%02X %02X %02X] %s\r\n",
                 deltaUs, pkt.header, pkt.byte1, pkt.byte2, pkt.byte3, tag);
}

void setup() {
  Serial1.begin(TEST_BAUD, SERIAL_8N1, TEST_RX_PIN, TEST_TX_PIN);
  Serial1.println("\n[test_midi] minimal USB MIDI RX test v2 booting...");

#if MIDI_DIAG_ENABLED
  Serial1.println("[test_midi] MIDI_DIAG=1: stack (DCD/USBD/class) events follow [diag] lines.");
#endif

  midiRx.begin();
  USB.begin();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // on = running

  Serial1.println("[test_midi] ready. RX d= is microseconds since previous packet.");
  Serial1.println("[test_midi] Chord test: all chord notes should arrive within ~1ms.");
}

void loop() {
  int64_t nowUs = esp_timer_get_time();

#if MIDI_DIAG_ENABLED
  // --- drain device-stack diagnostic ring, print from here (never in ISR) --
  {
    static midi_diag_evt_t evts[24];
    static const char *K[] = {"?",     "DCD_ARM", "DCD_RXPKT", "DCD_DONE", "USBD_ISRQ", "USBD_DISP",
                              "DROP",  "MIDI_CB", "MIDI_RX",   "MIDI_ARM", "STRM_BLK",  "STRM_CLAIM",
                              "STRM_SHRINK", "STRM_ARM"};
    static int64_t lastDiagT = 0;
    uint32_t n = midi_diag_read(evts, 24);
    // print only the RX path (OUT endpoints); re-arm/claim/block lines are
    // exactly the interesting ones below the DCD
    for (uint32_t i = 0; i < n; i++) {
      const midi_diag_evt_t &e = evts[i];
      if (e.kind >= sizeof(K) / sizeof(K[0])) continue;
      Serial1.printf("[diag] +%-8lldus %-12s ep=%02X len=%-5u aux=%lu\r\n",
                     (long long)(e.t - lastDiagT), K[e.kind], e.ep,
                     (unsigned)e.len, (unsigned long)e.aux);
      lastDiagT = e.t;
    }
  }
#endif

  // --- USB mount transitions (edge-detected every loop pass) --------------
  bool mounted = tud_mounted();
  if (mounted != lastMounted) {
    lastMounted = mounted;
    if (mounted) {
      everMounted = true;
      mountAtUs = nowUs;
      Serial1.printf("USB mounted (enumerated) t=%.3fs\r\n", nowUs / 1e6);
    } else {
      Serial1.printf("USB UNMOUNTED t=%.3fs (was up %.3fs)\r\n",
                     nowUs / 1e6, (nowUs - mountAtUs) / 1e6);
    }
  }
  // One-shot hint if still unmounted 15 s after boot.
  static bool unmountHintDone = false;
  if (!everMounted && !unmountHintDone && nowUs > 15000000LL) {
    unmountHintDone = true;
    Serial1.println("USB never mounted: replug cable (or EN reset), try another "
                    "port/cable, check Device Manager for 'TinyUSB MIDI'.");
  }

  midiEventPacket_t pkt {0, 0, 0, 0};
  uint32_t avail = tud_midi_available(); // FIFO bytes ready RIGHT NOW
  uint8_t batch[4 * 16];                 // drain up to 16 packets per poll
  uint32_t nb = tud_midi_packet_read_n(batch, 16);
  if (nb > 0) {
    // avail was measured on the poll that popped this batch: with a healthy
    // stack a one-transfer chord batch shows q ~= 4*nb. If the endpoint was
    // stalled, the batch dump shows exactly what had been queued.
    for (uint32_t i = 0; i < nb; i++) {
      const uint8_t *p = &batch[4 * i];
      int64_t delta = lastRxUs == 0 ? 0 : nowUs - lastRxUs;
      if (lastRxUs != 0) {
        if (delta > 1000) slowCount++;
        if (delta > maxDeltaUs) maxDeltaUs = delta;
      }
      rxCount++;
      char tag[24] = "";
      if (lastRxUs == 0)
        strcpy(tag, "(first)");
      else if (delta > 2000)
        strcpy(tag, "(>2ms!)");
      else if (delta > 1000)
        strcpy(tag, "(>1ms)");
      if (i == 0 && nb > 1)
        strcat(tag, nb > 8 ? "(BATCH!)" : "(batch)");
      Serial1.printf("RX d=%-9lld q=%-3u hdr=%02X [%02X %02X %02X] %s\r\n",
                     delta, (unsigned)avail, p[0], p[1], p[2], p[3], tag);
      lastRxUs = nowUs;
    }
  }

  // --- TX probe (compact; only when mounted so the FIFO stays clean) ------
  if (TX_PROBE_MS > 0 && mounted) {
    if (!txActive && nowUs >= nextTxAtUs) {
      txNote = 60 + (uint8_t)(txCount % 12);
      midiRx.noteOn(txNote, 100, MIDI_CH);
      txActive = true;
      txOnAtUs = nowUs;
      txCount++;
    } else if (txActive && nowUs - txOnAtUs >= TX_HOLD_MS * 1000LL) {
      midiRx.noteOff(txNote, 0, MIDI_CH);
      txActive = false;
      nextTxAtUs = nowUs + TX_PROBE_MS * 1000LL;
    }
  }

  // --- heartbeat (compact, every 5 s) ------------------------------------
  static int64_t lastStatUs = 0;
  if (nowUs - lastStatUs >= 5000000LL) {
    uint32_t statAvail = tud_midi_available();
    Serial1.printf("[stat] t=%llds rx=%lu slow(>1ms)=%lu maxgap=%lldus tx=%lu usb=%s\r\n",
                   nowUs / 1000000, rxCount, slowCount, maxDeltaUs,
                   txCount,
                   mounted ? "MOUNTED" : (everMounted ? "DROPPED(was mounted)" :
                                            "NOT-MOUNTED"));
    if (statAvail != 0)
      Serial1.printf("[stat] NOTE: %lu bytes left in FIFO at stat time\r\n",
                     (unsigned long)statAvail);
    lastStatUs = nowUs;
  }
}
