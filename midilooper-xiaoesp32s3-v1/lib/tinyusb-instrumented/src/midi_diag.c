#include "midi_diag.h"

#if MIDI_DIAG_ENABLED

#include "freertos/FreeRTOS.h"
#include "portmacro.h"
#include "esp_timer.h"

#define MD_RING 128
#define MD_MASK (MD_RING - 1)

// Raw EP4 receive-event ring (64 entries)
#define MD_EP4_RING 64
#define MD_EP4_MASK (MD_EP4_RING - 1)

static portMUX_TYPE md_mux = portMUX_INITIALIZER_UNLOCKED;
static midi_diag_evt_t md_ring[MD_RING];
static volatile uint32_t md_wr = 0;
static volatile uint32_t md_rd = 0;

static midi_diag_ep4_evt_t md_ep4_ring[MD_EP4_RING];
static volatile uint32_t md_ep4_wr = 0;
static volatile uint32_t md_ep4_rd = 0;

void midi_diag_log(uint32_t kind, uint32_t len, uint32_t ep, uint32_t aux) {
  portENTER_CRITICAL_SAFE(&md_mux);
  midi_diag_evt_t *e = &md_ring[md_wr & MD_MASK];
  e->t = esp_timer_get_time();
  e->aux = aux;
  e->kind = kind;
  e->len = len;
  e->ep = ep;
  e->p0 = 0;
  e->p1 = 0;
  md_wr++;
  portEXIT_CRITICAL_SAFE(&md_mux);
}

void midi_diag_log_payload(uint32_t kind, uint32_t len, uint32_t ep, uint32_t aux,
                           uint32_t p0, uint32_t p1) {
  portENTER_CRITICAL_SAFE(&md_mux);
  midi_diag_evt_t *e = &md_ring[md_wr & MD_MASK];
  e->t = esp_timer_get_time();
  e->aux = aux;
  e->kind = kind;
  e->len = len;
  e->ep = ep;
  e->p0 = p0;
  e->p1 = p1;
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

void midi_diag_ep4_record(uint16_t type, uint32_t grxstsp_raw, uint32_t doepctl,
                          uint32_t doeptsiz, uint32_t doepint) {
  portENTER_CRITICAL_SAFE(&md_mux);
  midi_diag_ep4_evt_t *e = &md_ep4_ring[md_ep4_wr & MD_EP4_MASK];
  e->t = esp_timer_get_time();
  e->grxstsp = grxstsp_raw;
  e->doepctl = doepctl;
  e->doeptsiz = doeptsiz;
  e->doepint = doepint;
  e->type = type;
  e->pad = 0;
  md_ep4_wr++;
  portEXIT_CRITICAL_SAFE(&md_mux);
}

uint32_t midi_diag_ep4_read(midi_diag_ep4_evt_t *out, uint32_t max) {
  uint32_t n = 0;
  portENTER_CRITICAL(&md_mux);
  while (md_ep4_rd != md_ep4_wr && n < max) {
    out[n++] = md_ep4_ring[md_ep4_rd & MD_EP4_MASK];
    md_ep4_rd++;
  }
  portEXIT_CRITICAL(&md_mux);
  return n;
}

uint32_t midi_diag_ep4_pending(void) {
  return md_ep4_wr - md_ep4_rd;
}

#endif // MIDI_DIAG_ENABLED
