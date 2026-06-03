import { ref } from 'vue';
import { api, setToken, token } from './api.js';
import { disconnectLive } from './liveState.js';

/* null = status not yet known. The view layer must NOT guess a value here:
 * defaulting to `configured:false` made the UI paint the Setup form before the
 * device had answered, which on a poor link flashed for seconds and then
 * snapped to the dashboard. Stay null until /api/auth/status actually resolves;
 * App.vue shows the boot spinner meanwhile and retries. */
export const authStatus = ref(null);

export async function refreshStatus() {
  /* On failure, leave the previous value untouched rather than wiping it to a
   * fabricated "unconfigured": on first load that keeps the spinner up (caller
   * retries); on a post-action refresh it avoids a transient timeout dumping an
   * authenticated user back to Setup/Login. */
  try { authStatus.value = await api.get('/api/auth/status'); }
  catch { /* keep prior status; caller decides whether to retry */ }
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
