// =============================================================================
//  midi_diag.h - TinyUSB MIDI RX instrumentation (DCD / USBD / MIDI class)
//
//  Shared by the instrumented TinyUSB sources in lib/tinyusb-instrumented/src
//  (which replace the framework's prebuilt libarduino_tinyusb.a objects) and
//  by the application that drains and prints the captured events.
//
//  Events are captured into a small lock-free-ish ring buffer with a single
//  microsecond timestamp source (esp_timer_get_time), so nothing is printed
//  from interrupt context. Enable with the build flag -DMIDI_DIAG=1; with the
//  flag absent every hook compiles to nothing.
// =============================================================================
#ifndef MIDI_DIAG_H
#define MIDI_DIAG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MIDI_DIAG) && MIDI_DIAG
#define MIDI_DIAG_ENABLED 1
#else
#define MIDI_DIAG_ENABLED 0
#endif

// Event kinds (kind field of midi_diag_evt_t)
enum {
  DIAG_DCD_ARM       = 1,  // dcd_edpt_xfer() armed an OUT transfer: len=total_bytes ep=epnum
  DIAG_DCD_RXPKT     = 2,  // DWC2 slave RXFLVL: OUT packet popped off the RxFIFO: len=byte_count ep=epnum aux=adjusted total_len
  DIAG_DCD_DONE      = 3,  // DWC2 OUT XFERCOMPL: event queued to the device stack: len=total_len ep=epnum
  DIAG_USBD_ISRQ     = 4,  // usbd dcd_event_handler() sees XFER_COMPLETE: len=len ep=ep_addr aux=1 if class xfer_isr handled it inline
  DIAG_USBD_DISP     = 5,  // usbd task: XFER_COMPLETE dispatched to the class xfer_cb: len=len ep=ep_addr
  DIAG_USBD_DROP     = 6,  // XFER_COMPLETE event dropped, device event queue full: len=len ep=ep_addr
  DIAG_MIDI_CB       = 7,  // midid_xfer_cb() entered: ep=ep_addr len=xferred_bytes aux=result
  DIAG_MIDI_RX       = 8,  // bytes pushed into the MIDI RX stream FIFO: len=xferred_bytes ep=ep_addr aux=fifo_count after push
  DIAG_MIDI_ARM      = 9,  // MIDI class re-armed the OUT endpoint: len=armed_bytes (0 = blocked) ep=ep_addr aux=fifo_count (bytes not yet read)
  DIAG_STREAM_BLOCK  = 10, // tu_edpt_stream_read_xfer() refused: FIFO remaining < mps (FIFO must be empty to re-arm): len=mps ep=ep_addr aux=remaining
  DIAG_STREAM_CLAIM  = 11, // tu_edpt_claim() failed (e.g. RX_PENDING hold, endpoint busy): len=0 ep=ep_addr aux=fifo_remaining
  DIAG_STREAM_SHRINK = 12, // claim succeeded but FIFO cannot fit one full packet: transfer released: len=remaining ep=ep_addr
  DIAG_STREAM_ARM    = 13, // tu_edpt_stream_read_xfer() armed the endpoint: len=count ep=ep_addr
  DIAG_DCD_OUT_REGS  = 14, // DWC2 slave OUT XFERCOMPL: event queued to the device stack: len=total_len ep=epnum aux=registers (ctl/tsiz/dma)
  DIAG_DCD_IRQ       = 15, // DCD interrupt handler entered
  DIAG_DCD_OEP       = 16, // DCD OUT endpoint interrupt
  DIAG_DCD_RXSTS     = 17, // DCD RX status FIFO entry
};

// Captured diagnostic event. len/ep/aux interpretation depends on kind.
typedef struct {
  int64_t  t;    // esp_timer_get_time() in microseconds (single common time base)
  uint32_t aux;  
  uint32_t kind;
  uint32_t len;
  uint32_t ep;   // full endpoint address (dir bit included where applicable)
  uint32_t pad;
} midi_diag_evt_t;

#if MIDI_DIAG_ENABLED

// Call from any context (task or ISR); never blocks, never prints.
void midi_diag_log(uint32_t kind, uint32_t len, uint32_t ep, uint32_t aux);

// Pop up to max events into out (task context only). Returns the number popped.
uint32_t midi_diag_read(midi_diag_evt_t *out, uint32_t max);

// Number of unconsumed events.
uint32_t midi_diag_pending(void);

#else

#define midi_diag_log(...) ((void)0)          // arguments not evaluated
#define midi_diag_read(...) ((uint32_t)0)
#define midi_diag_pending() ((uint32_t)0)

#endif

#ifdef __cplusplus
}
#endif

#endif // MIDI_DIAG_H
