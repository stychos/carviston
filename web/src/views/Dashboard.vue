<script setup>
import { ref, computed, onBeforeUnmount } from 'vue';
import Button from 'primevue/button';
import Dialog from 'primevue/dialog';
import Password from 'primevue/password';
import Message from 'primevue/message';
import { useToast } from 'primevue/usetoast';

import { state, connected } from '../composables/liveState.js';
import { api, HttpError } from '../composables/api.js';
import { authStatus, login, logout } from '../composables/auth.js';
import { connectLive } from '../composables/liveState.js';
import ConfigModal from '../components/ConfigModal.vue';

const toast = useToast();
const configOpen = ref(false);

/* Settings button → login modal flow when the user is on an unlocked
 * dashboard. ConfigModal opens only after auth succeeds. */
const loginOpen = ref(false);
const loginPw   = ref('');
const loginErr  = ref('');
const loginBusy = ref(false);

function openSettings() {
  if (authStatus.value.authenticated) {
    configOpen.value = true;
  } else {
    loginPw.value  = '';
    loginErr.value = '';
    loginOpen.value = true;
  }
}
async function submitLogin() {
  loginErr.value = '';
  loginBusy.value = true;
  try {
    await login(loginPw.value);
    /* Reconnect WS under the new token so the per-frame re-auth (used
     * when dashboard is locked) sees an authenticated session. */
    connectLive();
    loginOpen.value = false;
    configOpen.value = true;
  } catch {
    loginErr.value = 'Wrong password.';
  } finally {
    loginBusy.value = false;
  }
}

/* --- Matter commissioning card --------------------------------------------
 * State is driven by GET /api/matter/code (active / qr / manual / window_s).
 * When the user clicks "Pair with Matter" we POST /api/matter/open then
 * poll the code endpoint every 2 s until the window closes (timeout or
 * pairing complete). Polling stops on close so the dashboard isn't chatty
 * when Matter is idle. */
const matter = ref({ active: false, qr: '', manual: '', window_s_remaining: 0 });
const matterSupported = ref(true);   /* flipped false on 404 — Matter disabled at build */
const matterBusy = ref(false);
let matterPollHandle = null;

async function refreshMatter() {
  try {
    matter.value = await api.get('/api/matter/code');
  } catch (e) {
    if (e instanceof HttpError && e.status === 404) {
      matterSupported.value = false;
      stopMatterPolling();
    }
  }
}
function startMatterPolling() {
  if (matterPollHandle) return;
  matterPollHandle = setInterval(async () => {
    await refreshMatter();
    if (!matter.value.active) stopMatterPolling();
  }, 2000);
}
function stopMatterPolling() {
  if (matterPollHandle) { clearInterval(matterPollHandle); matterPollHandle = null; }
}
async function openMatterPairing() {
  matterBusy.value = true;
  try {
    await api.post('/api/matter/open', { window_s: 300 });
    await refreshMatter();
    startMatterPolling();
    toast.add({ severity: 'success', summary: 'Pairing window open',
                detail: 'Scan the QR or enter the manual code in Home/Google Home',
                life: 4000 });
  } catch (e) {
    if (e instanceof HttpError && (e.status === 404 || e.status === 503)) {
      matterSupported.value = false;
    } else {
      toast.add({ severity: 'error', summary: 'Could not open pairing window',
                  detail: e.message, life: 4000 });
    }
  } finally {
    matterBusy.value = false;
  }
}
async function copyToClipboard(text) {
  try {
    await navigator.clipboard.writeText(text);
    toast.add({ severity: 'info', summary: 'Copied', life: 1500 });
  } catch {
    /* clipboard may be blocked over plain HTTP — fall back to a manual prompt */
    window.prompt('Copy this:', text);
  }
}
/* Probe the endpoint once at mount so the card hides itself in builds where
 * Matter is compiled out — the firmware stub returns 404 (registered route
 * yields ESP_OK but the stub fills active=false; if route isn't registered
 * we'll see 404). Either way: empty state + supported flag govern rendering. */
refreshMatter();
onBeforeUnmount(stopMatterPolling);

const TEMPS = [40, 50, 60, 70, 80];
const MODE_NAMES = ['Super-fast', 'Fast', 'Optimal', 'Eco'];

/* safety_label and safety_class both derive from the same source so they
 * stay consistent before state arrives (the earlier `?? 0` fallback in the
 * label diverged from the strict `=== 0` check in the class, producing
 * "Safety: OK" with a red dot on first render). */
const SAFETY_LABELS = ['OK', 'Cutoff tripped', 'Probe fault', 'Over-temp', 'No probes'];
const safety_label = computed(() => {
  const s = state.value?.safety;
  if (s == null) return '…';
  return SAFETY_LABELS[s] || 'unknown';
});
const safety_class = computed(() => {
  const s = state.value?.safety;
  if (s == null) return '';
  return s === 0 ? 'good' : 'bad';
});

const water = computed(() => {
  const w = state.value?.water_c;
  if (w == null) return '—';
  return w.toFixed(1);
});

const anyHeater = computed(() => state.value?.heater_active?.some(Boolean));
const wifiPill = computed(() => {
  if (!state.value) return { label: '…', cls: '' };
  /* STA wins when connected — show SSID + RSSI. */
  if (state.value.wifi_ssid) {
    return { label: `${state.value.wifi_ssid} · ${state.value.wifi_rssi} dBm`, cls: 'good' };
  }
  /* AP up (pure AP or APSTA hybrid pre-STA-IP) — full signal, show AP SSID. */
  if (state.value.wifi_ap_ssid) {
    return { label: `AP · ${state.value.wifi_ap_ssid}`, cls: 'good' };
  }
  return { label: 'offline', cls: 'warn' };
});

async function setTarget(c) {
  try { await api.post('/api/heater/target', { celsius: c }); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
async function setMode(m) {
  if (state.value?.mode === m) return;
  try { await api.post('/api/heater/mode', { mode: m }); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
async function togglePower() {
  try { await api.post('/api/heater/power'); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
async function clearSafety() {
  try { await api.post('/api/safety/clear'); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
</script>

<template>
  <div class="shell">
    <div class="app-header">
      <div class="app-title">Carviston</div>
      <div class="row">
        <span class="pill" :class="connected ? 'good' : 'warn'">
          <span class="dot"></span>{{ connected ? 'live' : 'polling' }}
        </span>
        <Button icon="pi pi-cog" text rounded @click="openSettings" />
        <Button v-if="authStatus.authenticated" icon="pi pi-sign-out" text rounded @click="logout" />
      </div>
    </div>

    <div class="stack">
      <div class="tile">
        <h3>Water temperature</h3>
        <div class="spaced">
          <div class="big-num">{{ water }}<span style="font-size: 18px; color: var(--muted);">°C</span></div>
          <span class="pill" :class="state?.shower_ready ? 'good' : ''">
            <span class="dot"></span>Shower {{ state?.shower_ready ? 'ready' : 'cold' }}
          </span>
        </div>
        <div class="row" style="margin-top: 10px; flex-wrap: wrap; gap: 6px;">
          <span v-for="(p, i) in state?.probes || []" :key="i" class="pill"
                :class="p.fault ? 'bad' : 'good'">
            <span class="dot"></span>
            Probe {{ i === 0 ? 'A' : 'B' }}:
            {{ p.fault ? 'fault' : `${p.regulation_c.toFixed(1)}°C / ${p.safety_c.toFixed(1)}°C` }}
          </span>
        </div>
      </div>

      <div class="tile">
        <h3>Target</h3>
        <div class="temp-row">
          <button v-for="t in TEMPS" :key="t" type="button"
                  class="temp-step" :class="{ active: state?.target_c === t }"
                  @click="setTarget(t)">
            {{ t }}°
          </button>
        </div>

        <label class="muted" style="display: block; margin-top: 16px; font-size: 12px;">Mode</label>
        <div class="mode-row">
          <button v-for="(name, idx) in MODE_NAMES" :key="idx" type="button"
                  class="mode-step" :class="{ active: state?.mode === idx }"
                  @click="setMode(idx)">
            {{ name }}
          </button>
        </div>

        <div class="muted" style="margin-top: 10px; font-size: 13px;">
          Phase: {{ state?.phase || '—' }}
          <template v-if="state?.phase_seconds_left">
            · {{ Math.ceil(state.phase_seconds_left / 60) }} min left
          </template>
        </div>
      </div>

      <div class="tile">
        <h3>Heaters</h3>
        <div class="h-status">
          <div class="h" :class="state?.heater_active?.[0] ? 'on' : 'off'">
            <span>Heater 1</span>
            <strong>{{ state?.heater_active?.[0] ? 'ON' : 'idle' }}</strong>
          </div>
          <div class="h" :class="state?.heater_active?.[1] ? 'on' : 'off'">
            <span>Heater 2</span>
            <strong>{{ state?.heater_active?.[1] ? 'ON' : 'idle' }}</strong>
          </div>
        </div>
        <div style="margin-top: 12px;">
          <Button :label="state?.master_enabled ? 'Turn heaters OFF' : 'Turn heaters ON'"
                  :severity="state?.master_enabled ? 'danger' : 'success'"
                  fluid @click="togglePower" />
        </div>
      </div>

      <div class="tile">
        <h3>System</h3>
        <div class="row" style="flex-wrap: wrap; gap: 8px;">
          <span class="pill" :class="wifiPill.cls">
            <span class="dot"></span>Wi-Fi: {{ wifiPill.label }}
          </span>
          <span class="pill" :class="safety_class">
            <span class="dot"></span>Safety: {{ safety_label }}
          </span>
        </div>
        <Button v-if="state && state.safety !== 0"
                label="Clear safety latch" size="small" severity="warn" text
                style="margin-top: 8px;" @click="clearSafety" />
      </div>

      <div v-if="matterSupported" class="tile">
        <h3>Matter</h3>
        <template v-if="!matter.active">
          <p class="muted" style="margin: 0 0 10px 0; font-size: 13px;">
            Pair Carviston with Apple Home, Google Home, or any Matter-compatible
            controller. Opens a 5-minute commissioning window.
          </p>
          <Button label="Pair with Matter" icon="pi pi-link"
                  severity="primary" :loading="matterBusy"
                  @click="openMatterPairing" />
        </template>
        <template v-else>
          <div class="row" style="justify-content: space-between; align-items: baseline;">
            <span class="pill good"><span class="dot"></span>Pairing window open</span>
            <span class="muted" style="font-size: 12px;">
              expires in {{ matter.window_s_remaining }} s
            </span>
          </div>
          <div style="margin-top: 12px;">
            <div class="muted" style="font-size: 12px; margin-bottom: 4px;">Manual pairing code</div>
            <div class="row" style="gap: 8px; align-items: center;">
              <code class="matter-manual">{{ matter.manual || '—' }}</code>
              <Button icon="pi pi-copy" text rounded size="small"
                      :disabled="!matter.manual"
                      @click="copyToClipboard(matter.manual)" />
            </div>
          </div>
          <div style="margin-top: 10px;">
            <div class="muted" style="font-size: 12px; margin-bottom: 4px;">QR payload</div>
            <div class="row" style="gap: 8px; align-items: center;">
              <code class="matter-qr">{{ matter.qr || '—' }}</code>
              <Button icon="pi pi-copy" text rounded size="small"
                      :disabled="!matter.qr"
                      @click="copyToClipboard(matter.qr)" />
            </div>
            <div class="muted" style="font-size: 12px; margin-top: 6px;">
              Paste the QR string into Home / Google Home, or type the
              manual code on the controller's "add device" screen.
            </div>
          </div>
        </template>
      </div>
    </div>

    <ConfigModal v-model:visible="configOpen" />

    <Dialog v-model:visible="loginOpen" modal :draggable="false"
            :dismissableMask="true" header="Sign in to settings"
            :style="{ width: 'min(400px, 96vw)' }">
      <div class="stack" style="gap: 10px;">
        <p class="muted" style="margin: 0; font-size: 13px;">
          The dashboard is open, but settings need the admin password.
        </p>
        <Password v-model="loginPw" toggleMask :feedback="false" fluid
                  :inputProps="{ autofocus: true }"
                  @keydown.enter="submitLogin" />
        <Message v-if="loginErr" severity="error" :closable="false">{{ loginErr }}</Message>
        <Button label="Sign in" :loading="loginBusy" @click="submitLogin" />
      </div>
    </Dialog>
  </div>
</template>
