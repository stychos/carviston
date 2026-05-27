import { ref } from 'vue';
import { api, setToken, token } from './api.js';
import { disconnectLive } from './liveState.js';

export const authStatus = ref({ configured: false, authenticated: false, dashboard_locked: false });

export async function refreshStatus() {
  try { authStatus.value = await api.get('/api/auth/status'); }
  catch { authStatus.value = { configured: false, authenticated: false }; }
  return authStatus.value;
}

export async function setupPassword(password) {
  await api.post('/api/auth/setup', { password });
  const r = await api.post('/api/auth/login', { password });
  setToken(r.token);
  await refreshStatus();
}

export async function login(password) {
  // If a previous session left a WebSocket open (e.g. user reloaded the page
  // mid-session, or a re-login happened without a full nav), tear it down so
  // the new token gets a fresh handshake.
  disconnectLive();
  const r = await api.post('/api/auth/login', { password });
  setToken(r.token);
  await refreshStatus();
}

export async function logout() {
  // Order matters: stop the live channel BEFORE clearing the token so the
  // server-side WS slot sees a clean close while the bearer is still valid,
  // and so the polling fallback timer doesn't fire 401s after sign-out.
  disconnectLive();
  try { await api.post('/api/auth/logout'); } catch {}
  setToken('');
  await refreshStatus();
}

export { token };
