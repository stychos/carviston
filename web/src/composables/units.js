// Temperature unit (display-only). The device and config stay canonical
// Celsius; /api/state carries BOTH `<base>_c` and `<base>_f` for every reading,
// plus `display_unit` (0=C, 1=F) — the user's preference. This module just picks
// which to show and converts the few values the API doesn't pre-convert (config
// inputs and the Celsius target constants).

import { computed } from 'vue';
import { state } from './liveState.js';

export const isFahrenheit = computed(() => state.value?.display_unit === 1);
export const unitSuffix   = computed(() => (isFahrenheit.value ? '°F' : '°C'));

/* Pick the active-unit field from a dual-unit object, e.g.
 * pickTemp(state.value, 'water') → water_f or water_c (null-safe). */
export function pickTemp(obj, base) {
  if (!obj) return null;
  return obj[`${base}_${isFahrenheit.value ? 'f' : 'c'}`];
}

/* Format a dual-unit object's field with the unit suffix; '—' when null. */
export function fmtTemp(obj, base, digits = 1) {
  const v = pickTemp(obj, base);
  return v == null ? '—' : `${v.toFixed(digits)}${unitSuffix.value}`;
}

/* Conversions for values the API doesn't pre-convert. Absolute temperature vs a
 * temperature *difference* (hysteresis, mismatch span) — the latter has no 32°
 * offset. */
export const cToF  = (c) => c * 9 / 5 + 32;
export const fToC  = (f) => (f - 32) * 5 / 9;
export const dcToF = (c) => c * 9 / 5;
export const dfToC = (f) => f * 5 / 9;

/* A Celsius integer constant (e.g. a target step) shown in the active unit. */
export const stepLabel = (c) => (isFahrenheit.value ? Math.round(cToF(c)) : c);

/* Two-way display proxy for a Celsius config field edited in the active unit.
 * `form[key]` stays Celsius; the model reads/writes display units and clamps
 * back into the canonical [loC, hiC] range. `delta:true` uses offset-free
 * conversion. Returns refs for the converted min/max too. */
export function tempField(form, key, loC, hiC, { delta = false } = {}) {
  const toD   = delta ? dcToF : cToF;
  const fromD = delta ? dfToC : fToC;
  const model = computed({
    get: () => Math.round(isFahrenheit.value ? toD(form[key]) : form[key]),
    set: (v) => {
      if (v == null) return;
      const c = Math.round(isFahrenheit.value ? fromD(v) : v);
      form[key] = Math.min(hiC, Math.max(loC, c));
    },
  });
  const min = computed(() => Math.ceil(isFahrenheit.value ? toD(loC) : loC));
  const max = computed(() => Math.floor(isFahrenheit.value ? toD(hiC) : hiC));
  return { model, min, max };
}
