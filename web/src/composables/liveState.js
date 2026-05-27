// WebSocket subscription to /ws. Reconnects with backoff. Falls back to
// polling /api/state every 2 s if the WS fails.
import { ref } from 'vue';
import { api, token } from './api.js';

export const state = ref(null);
export const connected = ref(false);

let ws = null;
let pollTimer = null;
let backoff = 1000;

function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(async () => {
    try { state.value = await api.get('/api/state'); }
    catch { /* swallow */ }
  }, 2000);
}
function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

export function connectLive() {
  if (ws) return;
  /* Token is included when present; backend accepts an empty token when
   * the dashboard is unlocked (default). With the dashboard locked, an
   * empty token fails the handshake and we fall through to polling — which
   * also 401s, so the UI just shows "polling" until the user signs in. */
  try {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = token.value
      ? `${proto}//${location.host}/ws?token=${encodeURIComponent(token.value)}`
      : `${proto}//${location.host}/ws`;
    ws = new WebSocket(url);
  } catch (e) {
    startPolling();
    return;
  }
  ws.onopen = () => {
    connected.value = true;
    backoff = 1000;
    stopPolling();
  };
  ws.onmessage = (ev) => {
    try { state.value = JSON.parse(ev.data); } catch {}
  };
  ws.onclose = () => {
    connected.value = false;
    ws = null;
    startPolling();
    setTimeout(connectLive, backoff);
    backoff = Math.min(backoff * 2, 15000);
  };
  ws.onerror = () => { try { ws.close(); } catch {} };
}

export function disconnectLive() {
  stopPolling();
  if (ws) {
    try { ws.close(); } catch {}
    ws = null;
  }
  connected.value = false;
  state.value = null;
}
