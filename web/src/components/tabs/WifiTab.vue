<script setup>
import { reactive, ref, computed, watch, onMounted } from 'vue';
import Select from 'primevue/select';
import InputText from 'primevue/inputtext';
import InputNumber from 'primevue/inputnumber';
import Password from 'primevue/password';
import Checkbox from 'primevue/checkbox';
import Button from 'primevue/button';
import Message from 'primevue/message';
import { useToast } from 'primevue/usetoast';
import { api, HttpError } from '../../composables/api.js';
import { state } from '../../composables/liveState.js';

const props = defineProps({
  cfg: Object,
  onSave: { type: Function, required: true },
});
const toast = useToast();

const WIFI_MODES = [
  { v: 0, l: 'Hotspot only' },
  { v: 1, l: 'Connect to existing network' },
];

const form = reactive({
  wifi_mode: props.cfg.wifi_mode,
  sta_ssid:  props.cfg.sta_ssid,
  sta_pass:  '',
  ap_ssid:   props.cfg.ap_ssid,
  ap_pass:   '',
  sta_fallback_enabled: props.cfg.sta_fallback_enabled ?? true,
  sta_fallback_seconds: props.cfg.sta_fallback_seconds ?? 60,
});
watch(() => props.cfg, c => Object.assign(form, {
  wifi_mode: c.wifi_mode, sta_ssid: c.sta_ssid, ap_ssid: c.ap_ssid,
  sta_fallback_enabled: c.sta_fallback_enabled ?? true,
  sta_fallback_seconds: c.sta_fallback_seconds ?? 60,
}));

const networks = ref([]);
const scanning = ref(false);
async function scan() {
  scanning.value = true;
  try { networks.value = await api.get('/api/wifi/scan'); }
  catch { networks.value = []; }
  finally { scanning.value = false; }
}
onMounted(scan);

const testing = ref(false);
const lastTested = ref(null);

const staInPlay = computed(() => form.wifi_mode === 1);
const apInPlay  = computed(() => form.wifi_mode === 0);

/* The connection test can only run while the device is serving an AP (AP mode,
 * or the STA-fallback recovery AP): testing new STA creds has to drop the STA
 * link, so from pure STA it would cut our own response. `apIsUp` reflects the
 * live runtime; it is `null` while live state hasn't loaded — we must NOT guess
 * from the saved mode there, because guessing "no AP" would silently drop the
 * must-test-first gate and let a wrong password get saved. */
const stateLoaded = computed(() => state.value != null);
const apIsUp     = computed(() =>
  stateLoaded.value ? !!state.value.wifi_ap_ssid : null);
const canTestNow = computed(() => staInPlay.value && apIsUp.value === true);

const testMatchesForm = computed(() => (
  !!lastTested.value
  && lastTested.value.ssid === form.sta_ssid
  && lastTested.value.pass === form.sta_pass
));
/* When we CAN test (an AP is up — typically first STA setup from the hotspot),
 * require a passing test before saving STA: that's what stops a wrong/blank
 * password from knocking the device off the network. When we can't test
 * (already on STA, no AP), allow the save — switching networks from STA can't
 * hold a link up to test, so the new creds apply on reconnect and the recovery
 * hotspot is the safety net if they're wrong. */
const needsTest = computed(() => canTestNow.value && !testMatchesForm.value);
const canSave   = computed(() => {
  if (!staInPlay.value) return true;        /* AP mode has no STA-test gate */
  if (!stateLoaded.value) return false;     /* AP status unknown — don't risk a lockout */
  return !needsTest.value;
});

/* Any edit to the credentials invalidates a prior test result. */
watch([() => form.sta_ssid, () => form.sta_pass], () => { lastTested.value = null; });

/* Switching to AP clears the STA credentials outright — they're irrelevant in
 * hotspot mode, and dropping them forces a fresh, tested entry on any later
 * switch back to STA instead of silently reusing stale values. */
watch(() => form.wifi_mode, (m) => {
  if (m === 0) { form.sta_ssid = ''; form.sta_pass = ''; lastTested.value = null; }
});

async function testCredentials() {
  if (!form.sta_ssid) {
    toast.add({ severity: 'warn', summary: 'Pick a network first', life: 2500 });
    return;
  }
  testing.value = true;
  try {
    const r = await api.post('/api/wifi/test', {
      ssid: form.sta_ssid,
      password: form.sta_pass,
      timeout_s: 12,
    });
    if (r.ok) {
      lastTested.value = { ssid: form.sta_ssid, pass: form.sta_pass };
      toast.add({ severity: 'success', summary: 'Connected — credentials work',
                  detail: 'You can save these settings now', life: 4000 });
    } else {
      lastTested.value = null;
      toast.add({ severity: 'error', summary: 'Could not connect',
                  detail: r.error || 'unknown reason', life: 4000 });
    }
  } catch (e) {
    lastTested.value = null;
    const detail = (e instanceof HttpError) ? e.message : (e?.message || String(e));
    toast.add({ severity: 'error', summary: 'Test failed', detail, life: 4000 });
  } finally {
    testing.value = false;
  }
}

async function saveNetwork() {
  if (!canSave.value) {
    toast.add({ severity: 'warn', summary: 'Test the connection first',
                detail: 'STA settings save only after a successful connection test.',
                life: 4000 });
    return;
  }
  const patch = { wifi_mode: form.wifi_mode };
  if (staInPlay.value) {
    /* Persist exactly the credentials we just tested. */
    patch.sta_ssid = form.sta_ssid;
    patch.sta_pass = form.sta_pass;
    patch.sta_fallback_enabled = !!form.sta_fallback_enabled;
    patch.sta_fallback_seconds = form.sta_fallback_seconds;
  } else {
    /* AP mode: explicitly clear the stored STA credentials. */
    patch.sta_ssid = '';
    patch.sta_pass = '';
    patch.ap_ssid  = form.ap_ssid;
    if (form.ap_pass) patch.ap_pass = form.ap_pass;
  }
  try {
    await props.onSave(patch);
    toast.add({ severity: 'success', summary: 'Saved', life: 2500 });
    form.sta_pass = ''; form.ap_pass = '';
    lastTested.value = null;
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}
</script>

<template>
  <div class="stack">
    <div>
      <label class="muted">Wi-Fi mode</label>
      <Select v-model="form.wifi_mode" :options="WIFI_MODES"
              optionLabel="l" optionValue="v" fluid />
    </div>

    <!-- ============================================================
         STA section (client mode + both mode)
         Left column: name + password + test button
         Right column: networks list (spans the full column height)
         ============================================================ -->
    <div v-if="staInPlay" class="wifi-sta-grid">
      <div class="wifi-sta-form">
        <div>
          <label class="muted">Network name</label>
          <InputText v-model="form.sta_ssid" fluid />
        </div>
        <div>
          <label class="muted">Network password</label>
          <Password v-model="form.sta_pass" toggleMask :feedback="false"
                    placeholder="network password" fluid />
        </div>
        <div>
          <Button label="Test connection" icon="pi pi-bolt" fluid
                  severity="secondary" :loading="testing"
                  :disabled="!form.sta_ssid || !canTestNow"
                  @click="testCredentials" />
        </div>
      </div>

      <div class="wifi-networks">
        <div class="wifi-networks-list">
          <div v-for="n in networks" :key="n.ssid + n.channel"
               class="wifi-network-item"
               :class="{ selected: form.sta_ssid === n.ssid }"
               @click="form.sta_ssid = n.ssid">
            <span>{{ n.ssid }}</span>
            <span class="muted" style="float: right; font-size: 12px;">{{ n.rssi }} dBm</span>
          </div>
          <div v-if="!networks.length" class="muted" style="padding: 8px;">No networks yet.</div>
        </div>
        <div class="spaced" style="margin-top: 6px;">
          <label class="muted">Available networks</label>
          <Button label="Rescan" size="small" text :loading="scanning" @click="scan" />
        </div>
      </div>
    </div>

    <Message v-if="staInPlay && testMatchesForm" severity="success" :closable="false">
      Last test passed — safe to save.
    </Message>
    <Message v-else-if="needsTest" severity="warn" :closable="false">
      Test the connection before saving. Saving credentials that don't work
      knocks every Wi-Fi client off the hotspot while the device retries.
    </Message>
    <Message v-else-if="staInPlay && !stateLoaded" severity="info" :closable="false">
      Checking device status…
    </Message>
    <Message v-else-if="staInPlay && apIsUp === false" severity="info" :closable="false">
      Connected to a network, so a live test isn't available here. Saving
      applies the new details when the device reconnects — if they're wrong,
      the recovery hotspot comes up so you can fix them.
    </Message>

    <!-- ============================================================
         Fallback hotspot — client mode only
         ============================================================ -->
    <div v-if="form.wifi_mode === 1"
         style="border-top: 1px solid var(--border); padding-top: 12px;">
      <div class="row" style="gap: 10px; align-items: center;">
        <Checkbox v-model="form.sta_fallback_enabled" :binary="true" inputId="sta-fallback" />
        <label for="sta-fallback" style="cursor: pointer;">
          Enable fallback hotspot if the network can't be reached
        </label>
      </div>
      <p class="muted" style="margin: 4px 0 8px 0; font-size: 12px;">
        If the device can't get an IP within the timeout below, it turns on
        a recovery hotspot so you can always reach it to fix the settings.
      </p>
      <template v-if="form.sta_fallback_enabled">
        <label class="muted">Timeout before falling back (seconds)</label>
        <InputNumber v-model="form.sta_fallback_seconds"
                     :min="10" :max="600" fluid showButtons />
      </template>
    </div>

    <!-- ============================================================
         AP section (hotspot mode + both mode)
         Two columns 50/50: name | password
         "leave blank" hints below their inputs.
         ============================================================ -->
    <div v-if="apInPlay" class="wifi-ap-grid">
      <div>
        <label class="muted">Hotspot name</label>
        <InputText v-model="form.ap_ssid" fluid />
        <p class="muted" style="font-size: 12px; margin: 4px 0 0 0;">
          Leave blank for the automatic name.
        </p>
      </div>
      <div>
        <label class="muted">Hotspot password</label>
        <Password v-model="form.ap_pass" toggleMask :feedback="false"
                  placeholder="leave blank to keep current" fluid />
        <p class="muted" style="font-size: 12px; margin: 4px 0 0 0;">
          Leave blank for no password.
        </p>
      </div>
    </div>

    <Button label="Save network settings" :disabled="!canSave" @click="saveNetwork" />
  </div>
</template>

<style scoped>
.wifi-sta-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  align-items: stretch;
}
.wifi-sta-form {
  display: flex; flex-direction: column; gap: 10px;
}
.wifi-networks {
  display: flex; flex-direction: column;
}
.wifi-networks-list {
  flex: 1;
  min-height: 180px;
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 4px;
}
.wifi-network-item {
  padding: 6px 8px;
  cursor: pointer;
  border-radius: 6px;
}
.wifi-network-item:hover { background: var(--surface-2); }
.wifi-network-item.selected { background: var(--surface-2); }

.wifi-ap-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}

@media (max-width: 520px) {
  .wifi-sta-grid,
  .wifi-ap-grid { grid-template-columns: 1fr; }
}
</style>
