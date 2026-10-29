#include "midi_diag.h"

#if MIDI_DIAG_ENABLED

#include "freertos/FreeRTOS.h"
#include "portmacro.h"
#include "esp_timer.h"

#define MD_RING 128
#define MD_MASK (MD_RING - 1)

static portMUX_TYPE md_mux = portMUX_INITIALIZER_UNLOCKED;
static midi_diag_evt_t md_ring[MD_RING];
static volatile uint32_t md_wr = 0;
static volatile uint32_t md_rd = 0;

void midi_diag_log(uint16_t kind, uint16_t len, uint16_t ep, uint32_t aux) {
  portENTER_CRITICAL_SAFE(&md_mux);
  midi_diag_evt_t *e = &md_ring[md_wr & MD_MASK];
  e->t = esp_timer_get_time();
  e->aux = aux;
  e->kind = kind;
  e->len = len;
  e->ep = ep;
  e->pad = 0;
  md_wr++;
  portEXIT_CRITICAL_SAFE(&md_mux);
}

uint32_t midi_diag_read(midi_diag_evt_t *out, uint32_t max) {
  uint32_t n = 0;
  portENTER_CRITICAL(&md_mux);
  while (md_rd != md_wr && n < max) {
    out[n++] = md_ring[md_rd & MD_MASK];
    md_rd++;
  }
  portEXIT_CRITICAL(&md_mux);
  return n;
}

uint32_t midi_diag_pending(void) {
  return md_wr - md_rd;
}

#endif // MIDI_DIAG_ENABLED
