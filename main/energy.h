/* energy — approximate electrical-energy accounting for the heating elements.
 *
 * Independent of the heating "programs" tracked by benchmark.c: energy accrues
 * whenever a relay is closed, including during routine maintenance cycling. We
 * integrate (relay on-time × per-element watts) every control tick and bucket
 * the result by LOCAL calendar day in a small NVS-backed ring, so the UI can
 * show consumption for today / the last 7 days / the last 30 days plus a
 * lifetime total.
 *
 * Day bucketing needs a real wall clock (SNTP). Energy consumed before the
 * clock syncs is held in a pending accumulator and folded into the first known
 * day; the lifetime total always counts it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double today_wh;   /* current local day */
    double week_wh;    /* last 7 local days incl. today */
    double month_wh;   /* last 30 local days incl. today */
    double total_wh;   /* lifetime */
} energy_totals_t;

esp_err_t energy_init(void);

/* Called from the heater_control tick. Integrates the time since the previous
 * call against whichever elements are currently energised. `relay_on[i]` is the
 * commanded state of element i; `watts[i]` its rated power. */
void energy_account(const bool relay_on[2], const uint16_t watts[2]);

/* Snapshot the rolling totals. */
void energy_get(energy_totals_t *out);

/* Persist now (e.g. before a graceful reboot). */
void energy_flush(void);

/* Wipe all accumulated energy and persist the empty store. */
void energy_clear(void);

#ifdef __cplusplus
}
#endif
