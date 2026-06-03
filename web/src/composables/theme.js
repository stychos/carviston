// Theme manager — 'auto' | 'light' | 'dark', persisted to localStorage,
// default 'auto'. Applied by toggling `.dark-mode` on <html>, which drives
// BOTH our CSS-variable palette (`:root` light / `:root.dark-mode` dark) and
// PrimeVue's Aura `darkModeSelector: '.dark-mode'` — so the custom UI and every
// PrimeVue component switch together.
//
// A tiny inline script in index.html applies the same class before this bundle
// loads, so there's no light→dark (or dark→light) flash on first paint.

import { ref } from 'vue';

const KEY = 'carviston-theme';
const VALID = ['auto', 'light', 'dark'];

const stored = localStorage.getItem(KEY);
export const theme = ref(VALID.includes(stored) ? stored : 'auto');

const mql = window.matchMedia('(prefers-color-scheme: dark)');

/* The effective light/dark resolution: explicit choice, or the OS preference
 * when on 'auto'. Exposed so the UI can show which way 'auto' currently leans. */
export function isDark() {
  return theme.value === 'dark' || (theme.value === 'auto' && mql.matches);
}

function apply() {
  document.documentElement.classList.toggle('dark-mode', isDark());
}

export function setTheme(v) {
  if (!VALID.includes(v)) return;
  theme.value = v;
  try { localStorage.setItem(KEY, v); } catch { /* private mode — runtime only */ }
  apply();
}

/* Follow OS changes only while on 'auto'. */
mql.addEventListener('change', () => { if (theme.value === 'auto') apply(); });

apply();   // keep the class in sync with state at module load
