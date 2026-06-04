<script setup>
import { ref, computed, watch, onBeforeUnmount } from 'vue';
import Button from 'primevue/button';
import Dialog from 'primevue/dialog';
import Password from 'primevue/password';
import Message from 'primevue/message';
import { useToast } from 'primevue/usetoast';

import { state, connected, online } from '../composables/liveState.js';
import { api, HttpError } from '../composables/api.js';
import { authStatus, login, logout } from '../composables/auth.js';
import { connectLive } from '../composables/liveState.js';
import ConfigModal from '../components/ConfigModal.vue';
import { safetyLabel, safetyDescription } from '../composables/safety.js';
import { unitSuffix, pickTemp, fmtTemp, stepLabel } from '../composables/units.js';

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
 * "Safety: OK" with a red dot on first render). The enum→string maps live in
 * composables/safety.js, shared with the maintenance tab. */
const safety_label = computed(() => safetyLabel(state.value?.safety));
const safety_class = computed(() => {
  const s = state.value?.safety;
  if (s == null) return '';
  return s === 0 ? 'good' : 'bad';
});

const water = computed(() => {
  const w = pickTemp(state.value, 'water');
  if (w == null) return '—';
  return w.toFixed(1);
});
const targetDisplay = computed(() => {
  const t = pickTemp(state.value, 'target');
  return t == null ? '—' : Math.round(t);
});

/* Self-defensive temperature formatter: a faulted tank serializes its NTCs as
 * null (cJSON renders NaN as null), so don't trust the fault flag to gate the
 * .toFixed — guard on the value itself. */

const anyHeater = computed(() => state.value?.heater_active?.some(Boolean));

/* User-set display name (from /api/state, so the open dashboard sees it without
 * auth); falls back to the product name until the first sample arrives. */
const deviceName = computed(() => state.value?.device_name || 'Carviston');
watch(deviceName, (n) => { document.title = n; }, { immediate: true });

/* --- Temperature gauge geometry --------------------------------------------
 * The dial sweeps 270° (a 90° gap at the bottom). With pathLength="100" on the
 * SVG circles, 270° == 75 dash units, so the value arc is simply frac * 75.
 * Range 10–85 °C spans the lived-in band for a storage tank. */
const GAUGE_MIN = 10, GAUGE_MAX = 85, GAUGE_ARC = 75;
const clamp01 = (x) => Math.min(1, Math.max(0, x));
const tempFrac = (t) => clamp01((t - GAUGE_MIN) / (GAUGE_MAX - GAUGE_MIN));

const waterFault = computed(() => state.value != null && state.value.water_c == null);
/* "heating" = master on AND an element is genuinely firing (not just enabled). */
const heating   = computed(() => !!state.value?.master_enabled && !!anyHeater.value);

const gaugeDash = computed(() => {
  const w = state.value?.water_c;
  const frac = w == null ? 0 : tempFrac(w);
  return `${(frac * GAUGE_ARC).toFixed(2)} 100`;
});
/* Target tick: position on the same arc (135° start, clockwise). */
const targetDot = computed(() => {
  const t = state.value?.target_c;
  if (t == null) return null;
  const deg = 135 + tempFrac(t) * 270;
  const rad = (deg * Math.PI) / 180;
  return { x: 100 + 80 * Math.cos(rad), y: 100 + 80 * Math.sin(rad) };
});

/* --- Wi-Fi display ---------------------------------------------------------
 * STA connected → SSID with a signal-strength bar (RSSI mapped to 1..4).
 * AP (or STA recovery-AP) → AP SSID with the "AP" indicator (no signal bar;
 * the device IS the AP, signal strength isn't meaningful).
 * offline → empty-bar icon and a "Not connected" label. */
const RSSI_TO_LEVEL = (rssi) => {
  if (rssi == null || rssi >= 0) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
};
const SIGNAL_WORDS = ['none', 'weak', 'fair', 'good', 'excellent'];

const wifiMode = computed(() => {
  if (!state.value) return 'loading';
  if (state.value.wifi_ssid)     return 'sta';
  if (state.value.wifi_ap_ssid)  return 'ap';
  return 'offline';
});
const wifiName = computed(() => {
  if (!state.value) return 'Connecting…';
  if (state.value.wifi_ssid)    return state.value.wifi_ssid;
  if (state.value.wifi_ap_ssid) return state.value.wifi_ap_ssid;
  return 'Not connected';
});
const signalLevel = computed(() => RSSI_TO_LEVEL(state.value?.wifi_rssi));
const wifiSubtitle = computed(() => {
  switch (wifiMode.value) {
    case 'sta':
      return `${SIGNAL_WORDS[signalLevel.value]} signal · ${state.value.wifi_rssi} dBm`;
    case 'ap':
      return 'Hotspot mode — connect a phone or laptop to configure';
    case 'offline':
      return 'Not connected to a network';
    case 'loading':
      return 'Waiting for the device to respond…';
    default:
      return '';
  }
});

const safetyIconClass = computed(() => {
  const s = state.value?.safety;
  if (s == null) return 'pi pi-spin pi-spinner';
  return s === 0 ? 'pi pi-check-circle' : 'pi pi-exclamation-triangle';
});
const safetyStatusClass = computed(() => {
  const s = state.value?.safety;
  if (s == null) return 'sys-loading';
  return s === 0 ? 'sys-ok' : 'sys-bad';
});
const safetyTitle = computed(() => safetyLabel(state.value?.safety, 'Loading…'));
const safetyDescriptionText = computed(() =>
  safetyDescription(state.value?.safety, 'Waiting for first update…'));

/* These endpoints return the new state_json() in the response body. Assign
 * it back immediately so the UI reflects the change without waiting for
 * the next 1 Hz WebSocket push — keeps the active-button highlight
 * snappy even when the WS is mid-reconnect. */
async function setTarget(c) {
  try { state.value = await api.post('/api/heater/target', { celsius: c }); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
async function setMode(m) {
  if (state.value?.mode === m) return;
  try { state.value = await api.post('/api/heater/mode', { mode: m }); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
async function togglePower() {
  try { state.value = await api.post('/api/heater/power'); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}

/* Per-tank heater state (relay i ↔ tank i): 'heating' when its element is
 * firing, 'standby' when enabled but idle, 'off' when the element is disabled.
 * Display-only — the mode state machine owns which enabled element fires. */
function tankState(i) {
  if (!state.value?.element_enabled?.[i]) return 'off';
  return state.value?.heater_active?.[i] ? 'heating' : 'standby';
}
async function clearSafety() {
  try { state.value = await api.post('/api/safety/clear'); }
  catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
}
</script>

<template>
  <div class="shell">
    <div class="app-header">
      <div class="app-title">{{ deviceName }}</div>
      <div class="row">
        <span class="pill" :class="online ? (connected ? 'good' : 'warn') : 'bad'">
          <span class="dot"></span>{{ online ? (connected ? 'live' : 'polling') : 'offline' }}
        </span>
        <Button icon="pi pi-cog" text rounded @click="openSettings" />
        <Button v-if="authStatus.authenticated" icon="pi pi-sign-out" text rounded @click="logout" />
      </div>
    </div>

    <div class="stack">
      <div class="tile hero">
        <div class="gauge" :class="{ heating, fault: waterFault }">
          <svg class="gauge-svg" viewBox="0 0 200 200" aria-hidden="true">
            <circle class="gauge-track" cx="100" cy="100" r="80"
                    pathLength="100" stroke-dasharray="75 100"
                    transform="rotate(135 100 100)" />
            <circle class="gauge-value" cx="100" cy="100" r="80"
                    pathLength="100" :stroke-dasharray="gaugeDash"
                    transform="rotate(135 100 100)" />
            <circle v-if="targetDot" class="gauge-target" r="4"
                    :cx="targetDot.x" :cy="targetDot.y" />
          </svg>
          <div class="gauge-center">
            <div class="gauge-temp">{{ water }}<span class="gauge-unit">{{ unitSuffix }}</span></div>
            <div class="gauge-sub">target {{ targetDisplay }}°</div>
          </div>
        </div>

        <div class="hero-info">
          <div class="hero-head">
            <button type="button" class="power-toggle"
                    :class="{ on: state?.master_enabled }"
                    :disabled="!online || !state"
                    :title="state?.master_enabled
                              ? 'Heater is on — tap to turn off'
                              : 'Heater is off — tap to turn on'"
                    @click="togglePower">
              <i class="pi pi-power-off"></i>
              {{ state?.master_enabled ? 'On' : 'Off' }}
            </button>
            <span class="pill" :class="state?.shower_ready ? 'good' : ''">
              <span class="dot"></span>Shower {{ state?.shower_ready ? 'ready' : 'cold' }}
            </span>
          </div>

          <div class="hero-tanks">
            <div v-for="(p, i) in state?.tanks || []" :key="i" class="tank-row"
                 :title="p.fault ? 'sensor mismatch / fault' : ''">
              <span class="dot" :class="p.fault ? 'bad' : 'good'"></span>
              <span class="tank-name">{{ ['Inlet', 'Outlet'][i] || `Tank ${i + 1}` }}</span>
              <span class="tank-temps">{{ fmtTemp(p, 'regulation') }} / {{ fmtTemp(p, 'safety') }}</span>
              <span class="tank-state" :class="'state-' + tankState(i)">{{ tankState(i) }}</span>
            </div>
          </div>
        </div>
      </div>

      <div class="tile">
        <h3>Target</h3>
        <div class="temp-row">
          <button v-for="t in TEMPS" :key="t" type="button"
                  class="temp-step" :class="{ active: state?.target_c === t }"
                  :disabled="!state?.master_enabled"
                  @click="setTarget(t)">
            {{ stepLabel(t) }}°
          </button>
        </div>

        <label class="muted" style="display: block; margin-top: 16px; font-size: 12px;">Mode</label>
        <div class="mode-row">
          <button v-for="(name, idx) in MODE_NAMES" :key="idx" type="button"
                  class="mode-step" :class="{ active: state?.mode === idx }"
                  :disabled="!state?.master_enabled"
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

      <div class="tile-row">
        <div class="tile">
          <h3>Network</h3>
          <div class="sys-row">
            <div class="sys-icon-cell" :class="wifiMode === 'offline' ? 'sys-warn' : (wifiMode === 'loading' ? 'sys-loading' : 'sys-good')">
              <div v-if="wifiMode === 'ap'" class="sys-pi-large pi pi-wifi"></div>
              <div v-else-if="wifiMode === 'loading'" class="sys-pi-large pi pi-spin pi-spinner"></div>
              <div v-else class="signal-bars">
                <div class="bar" :class="{ on: signalLevel >= 1 }"></div>
                <div class="bar" :class="{ on: signalLevel >= 2 }"></div>
                <div class="bar" :class="{ on: signalLevel >= 3 }"></div>
                <div class="bar" :class="{ on: signalLevel >= 4 }"></div>
              </div>
            </div>
            <div class="sys-text">
              <div class="sys-title">{{ wifiName }}</div>
              <div class="sys-subtitle muted">{{ wifiSubtitle }}</div>
            </div>
          </div>
        </div>

        <div class="tile">
          <h3>Safety</h3>
          <div class="sys-row">
            <div class="sys-icon-cell" :class="safetyStatusClass">
              <i class="sys-pi-large" :class="safetyIconClass"></i>
            </div>
            <div class="sys-text">
              <div class="sys-title">{{ safetyTitle }}</div>
              <div class="sys-subtitle muted">{{ safetyDescriptionText }}</div>
            </div>
          </div>
        </div>
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
