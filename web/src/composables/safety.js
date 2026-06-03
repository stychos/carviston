// Single source of truth mapping the firmware `safety_status_t` enum
// (main/safety.c) to UI strings. Both the dashboard and the maintenance tab
// import this, so a change to the enum or its wording happens in one place
// instead of drifting between two hand-maintained copies.

export const SAFETY_LABELS = [
  'All clear',
  'Water too hot',
  'No temperature readings',
];

export const SAFETY_DESCRIPTIONS = [
  'Running normally',
  'A tank exceeded the over-temp limit — heating paused, resumes when it cools',
  'All sensors lost — heating paused, resumes when a sensor recovers',
];

/* `s` is the numeric safety code (state.safety). `fallback` is returned while
 * state hasn't arrived yet (s == null). */
export function safetyLabel(s, fallback = '…') {
  if (s == null) return fallback;
  return SAFETY_LABELS[s] || 'unknown';
}
export function safetyDescription(s, fallback = '') {
  if (s == null) return fallback;
  return SAFETY_DESCRIPTIONS[s] || '';
}
