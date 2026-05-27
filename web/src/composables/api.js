// Minimal fetch wrapper that injects the auth token and JSONs the body.
import { ref } from 'vue';

const TOKEN_KEY = 'cv.token';
export const token = ref(localStorage.getItem(TOKEN_KEY) || '');

export function setToken(t) {
  token.value = t || '';
  if (t) localStorage.setItem(TOKEN_KEY, t);
  else   localStorage.removeItem(TOKEN_KEY);
}

async function request(method, path, body) {
  const headers = { Accept: 'application/json' };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const sentToken = !!token.value;
  if (sentToken) headers['Authorization'] = 'Bearer ' + token.value;
  const r = await fetch(path, {
    method,
    headers,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  if (r.status === 401) {
    /* Only treat as session-expired if we actually sent a token. A 401 on
     * a settings endpoint without a token is just "this endpoint needs
     * auth, please sign in" — the dashboard is unlocked by default and
     * those calls deliberately go out with no token. */
    if (sentToken) setToken('');
    throw new HttpError(401, 'unauthorized');
  }
  const ct = r.headers.get('content-type') || '';
  const data = ct.includes('application/json') ? await r.json().catch(() => null) : await r.text();
  if (!r.ok) throw new HttpError(r.status, (data && data.error) || r.statusText);
  return data;
}

export class HttpError extends Error {
  constructor(status, msg) { super(msg); this.status = status; }
}

export const api = {
  get:  (p)        => request('GET',  p),
  post: (p, body)  => request('POST', p, body ?? {}),
  put:  (p, body)  => request('PUT',  p, body ?? {}),
};
