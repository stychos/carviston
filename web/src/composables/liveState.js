// Live state pipe.
//   - Polls /api/state every 2 s as the load-bearing transport. Works as
//     soon as auth status allows, and survives WS failures cleanly.
//   - WebSocket /ws is the low-latency overlay; while it's open we drop the
//     poll to a slow 10 s watchdog (covers a silently half-open socket).
// Either path writes the same `state` ref, so the UI doesn't care which
// one delivered the bytes.

import { ref } from 'vue';
import { api, token } from './api.js';

export const state = ref(null);
export const connected = ref(false);   /* WS transport open (live vs polling) */
/* Liveness based on *data recency*, not on the socket's belief about being
 * open. A board that loses power drops TCP without a close handshake, so the
 * browser's WS can linger "open" for minutes — `connected` alone would lie.
 * `online` flips false when no fresh sample (WS push or poll) has arrived
 * within STALE_MS, and back true the instant one does. */
export const online = ref(false);
const STALE_MS = 6000;

const POLL_FAST_MS = 2000;     /* no live socket — primary transport */
const POLL_SLOW_MS = 4000;     /* WS open — half-open-socket watchdog. Must stay
                                * below STALE_MS so a brief WS push gap is covered
                                * by a poll before `online` flips false. */
const BACKOFF_MIN_MS = 1000;
const BACKOFF_MAX_MS = 15000;
const STABLE_MS = 5000;        /* socket must stay open this long before we
                                * trust it enough to reset the backoff */

let ws = null;
let pollTimer = null;
let pollInterval = 0;
let reconnectTimer = null;
let stableTimer = null;
let backoff = BACKOFF_MIN_MS;
let started = false;
let watchdog = null;
let lastSeen = 0;
let pollWarned = false;        /* rate-limit the poll-failure console spam */

async function pollOnce() {
  try {
    state.value = await api.get('/api/state');
    markFresh();
    pollWarned = false;
  } catch (e) {
    /* 401 only happens when dashboard is locked + we have no token.
     * Leave state null so the UI can render the appropriate empty/login
     * affordance. Other errors (network, 5xx) are transient — log ONCE per
     * outage (not every 2 s) so a long offline stretch doesn't flood the
     * console, then keep trying quietly. */
    if (e?.status !== 401 && !pollWarned) {
      pollWarned = true;
      // eslint-disable-next-line no-console
      console.warn('state poll failed (will keep retrying):', e?.message || e);
    }
  }
}

/* Run the poll at `intervalMs`. Idempotent for a given interval. Primes an
 * immediate fetch only when the current sample is already stale, so a flapping
 * WebSocket (open→close repeatedly) doesn't fire a redundant GET on every
 * transition. */
function setPolling(intervalMs) {
  if (pollTimer && pollInterval === intervalMs) return;
  stopPolling();
  pollInterval = intervalMs;
  if (Date.now() - lastSeen >= intervalMs) pollOnce();
  pollTimer = setInterval(pollOnce, intervalMs);
}
function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  pollInterval = 0;
}

function markFresh() {
  lastSeen = Date.now();
  online.value = true;
}
function startWatchdog() {
  if (watchdog) return;
  lastSeen = Date.now();   /* grace: don't flag offline before the first sample */
  watchdog = setInterval(() => {
    if (Date.now() - lastSeen > STALE_MS) online.value = false;
  }, 1000);
}
function stopWatchdog() {
  if (watchdog) { clearInterval(watchdog); watchdog = null; }
}

function clearStableTimer() {
  if (stableTimer) { clearTimeout(stableTimer); stableTimer = null; }
}
function clearReconnect() {
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
}

function openWebSocket() {
  if (!started || ws) return;
  try {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = token.value
      ? `${proto}//${location.host}/ws?token=${encodeURIComponent(token.value)}`
      : `${proto}//${location.host}/ws`;
    ws = new WebSocket(url);
  } catch (e) {
    // eslint-disable-next-line no-console
    console.warn('WebSocket construct failed:', e?.message || e);
    return;
  }
  ws.onopen = () => {
    connected.value = true;
    /* Keep one /api/state every 10 s as a watchdog so a silently-broken WS
     * (TCP half-close the browser hasn't noticed) still refreshes the UI. */
    setPolling(POLL_SLOW_MS);
    /* Reset the reconnect backoff only once the socket has proven stable.
     * Resetting eagerly in onopen turns a server that keeps dropping us
     * (e.g. during a Wi-Fi mode flip that hops the AP channel) into a tight
     * 1 s reconnect loop — a request/log storm on a single-task device. */
    clearStableTimer();
    stableTimer = setTimeout(() => { backoff = BACKOFF_MIN_MS; }, STABLE_MS);
  };
  ws.onmessage = (ev) => {
    try { state.value = JSON.parse(ev.data); markFresh(); } catch {}
  };
  ws.onclose = () => {
    connected.value = false;
    ws = null;
    clearStableTimer();
    if (!started) return;   /* disconnectLive() ran — don't resurrect anything */
    /* Resume fast polling and retry the WS with capped exponential backoff. */
    setPolling(POLL_FAST_MS);
    clearReconnect();
    reconnectTimer = setTimeout(openWebSocket, backoff);
    backoff = Math.min(backoff * 2, BACKOFF_MAX_MS);
  };
  ws.onerror = () => { try { ws.close(); } catch {} };
}

export function connectLive() {
  if (started) {
    /* Already running — but force a WS re-open if it's idle (e.g. after
     * a token change). */
    if (!ws) openWebSocket();
    return;
  }
  started = true;
  /* setPolling() first, while lastSeen is still 0, so it primes an immediate
   * fetch on a cold start (the stale-check below sees no recent sample). On a
   * mid-session WS flap lastSeen is fresh, so onclose's setPolling() won't fire
   * a redundant prime. WS attempts in parallel. */
  setPolling(POLL_FAST_MS);
  startWatchdog();
  openWebSocket();
}

export function disconnectLive() {
  started = false;
  clearReconnect();
  clearStableTimer();
  stopPolling();
  stopWatchdog();
  if (ws) {
    try { ws.close(); } catch {}
    ws = null;
  }
  backoff = BACKOFF_MIN_MS;
  lastSeen = 0;            /* so the next connectLive() primes immediately */
  connected.value = false;
  online.value = false;
  state.value = null;
}
