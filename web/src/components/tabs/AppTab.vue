<script setup>
import { reactive, ref, watch } from 'vue';
import Select from 'primevue/select';
import InputText from 'primevue/inputtext';
import Checkbox from 'primevue/checkbox';
import Password from 'primevue/password';
import Button from 'primevue/button';
import { useToast } from 'primevue/usetoast';
import { api } from '../../composables/api.js';
import { refreshStatus } from '../../composables/auth.js';

const props = defineProps({
  cfg: Object,
  onSave: { type: Function, required: true },
});
const toast = useToast();

const POWER_LED_MODES = [
  { v: 0, l: 'Always on' },
  { v: 1, l: 'Only when a heater is active' },
];
const ECO_LED_MODES = [
  { v: 0, l: 'Wi-Fi state' },
  { v: 1, l: 'Eco mode indicator' },
];
const UNITS = [
  { v: 0, l: 'Celsius' },
  { v: 1, l: 'Fahrenheit' },
];

const form = reactive({
  power_led_mode: props.cfg.power_led_mode,
  eco_led_mode:   props.cfg.eco_led_mode,
  dashboard_unit: props.cfg.dashboard_unit,
});
watch(() => props.cfg, c => Object.assign(form, {
  power_led_mode: c.power_led_mode,
  eco_led_mode:   c.eco_led_mode,
  dashboard_unit: c.dashboard_unit,
}));

/* --- Device name. Saved on blur / Enter rather than per-keystroke. */
const deviceName = ref(props.cfg.device_name || '');
watch(() => props.cfg.device_name, v => { deviceName.value = v || ''; });
async function saveDeviceName() {
  const name = deviceName.value.trim();
  if (name === (props.cfg.device_name || '')) return;   /* nothing changed */
  try {
    await props.onSave({ device_name: name });
    toast.add({ severity: 'success', summary: 'Saved', life: 1200 });
  } catch (e) {
    deviceName.value = props.cfg.device_name || '';
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}

/* --- Network hostname (DHCP name on the router + <name>.local). Must be a DNS
 * label, so we coerce to lowercase [a-z0-9-] and strip edge hyphens, mirroring
 * the firmware's sanitize_hostname so the field shows exactly what gets stored.
 * Saving triggers a brief Wi-Fi reconnect (the name re-registers via DHCP). */
const hostname = ref(props.cfg.hostname || '');
watch(() => props.cfg.hostname, v => { hostname.value = v || ''; });
function normalizeHost(s) {
  return s.toLowerCase().replace(/[^a-z0-9-]+/g, '-').replace(/^-+|-+$/g, '');
}
async function saveHostname() {
  const h = normalizeHost(hostname.value);
  hostname.value = h;                                   /* reflect the cleanup in the field */
  if (h === (props.cfg.hostname || '')) return;         /* nothing changed */
  try {
    await props.onSave({ hostname: h });
    toast.add({ severity: 'success', summary: 'Saved — reconnecting Wi-Fi…', life: 1800 });
  } catch (e) {
    hostname.value = props.cfg.hostname || '';
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}

/* Auto-save on every Select change. The Select's @change fires only on
 * user interaction (not on the watch above that mirrors external cfg
 * updates), so we don't risk an echo loop. The backend coalesces rapid
 * NVS writes via app_config_save_deferred. */
async function autoSave(patch) {
  try {
    await props.onSave(patch);
    toast.add({ severity: 'success', summary: 'Saved', life: 1200 });
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}

/* --- Remember on/off across reboots / power cuts. */
const restorePower = ref(!!props.cfg.restore_power_on_boot);
watch(() => props.cfg.restore_power_on_boot, v => { restorePower.value = !!v; });
async function onRestorePowerChange(e) {
  const v = !!e.checked;
  try {
    await props.onSave({ restore_power_on_boot: v });
    toast.add({ severity: 'success', summary: 'Saved', life: 1200 });
  } catch (err) {
    restorePower.value = !v;
    toast.add({ severity: 'error', summary: 'Failed', detail: err.message, life: 3000 });
  }
}

/* --- Dashboard lock — moved from Maintenance into the password block. */
const dashboardLocked = ref(!!props.cfg.dashboard_locked);
watch(() => props.cfg.dashboard_locked, v => { dashboardLocked.value = !!v; });
async function onDashboardLockChange(e) {
  const v = !!e.checked;
  try {
    await props.onSave({ dashboard_locked: v });
    await refreshStatus();
    toast.add({
      severity: 'success',
      summary: v ? 'Dashboard now requires password' : 'Dashboard is now open',
      life: 2500,
    });
  } catch (err) {
    dashboardLocked.value = !v;
    toast.add({ severity: 'error', summary: 'Failed', detail: err.message, life: 3000 });
  }
}

/* --- Password change. Backend revokes every session on success, so we
 * reload the page to land on a fresh auth state — Dashboard if the
 * dashboard is unlocked, Login screen if it's locked. */
const oldPw = ref('');
const newPw = ref('');
async function changePassword() {
  if (!newPw.value || newPw.value.length < 4) {
    toast.add({ severity: 'warn', summary: 'New password too short', life: 2500 });
    return;
  }
  try {
    await api.post('/api/auth/password', { old: oldPw.value, new: newPw.value });
    toast.add({ severity: 'success', summary: 'Password updated — signing out…', life: 1500 });
    oldPw.value = ''; newPw.value = '';
    /* Brief delay so the toast is visible before the navigation. */
    setTimeout(() => {
      window.location.replace('/?v=' + Date.now());
    }, 1200);
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}
</script>

<template>
  <div class="stack">
    <div class="app-grid-2">
      <div>
        <label class="muted">Device name</label>
        <InputText v-model="deviceName" fluid maxlength="31" placeholder="Carviston"
                   @blur="saveDeviceName" @keyup.enter="saveDeviceName" />
        <p class="muted" style="margin: 4px 0 0 0; font-size: 12px;">
          Shown in the dashboard header and browser tab.
        </p>
      </div>

      <div>
        <label class="muted">Network hostname</label>
        <InputText v-model="hostname" fluid maxlength="31" placeholder="carviston"
                   @blur="saveHostname" @keyup.enter="saveHostname" />
        <p class="muted" style="margin: 4px 0 0 0; font-size: 12px;">
          The device's name on your router and at
          <code>{{ (hostname || 'carviston') }}.local</code>. Letters, numbers
          and hyphens only.
        </p>
      </div>
    </div>

    <div class="app-grid-3">
      <div>
        <label class="muted">Temperature unit</label>
        <Select v-model="form.dashboard_unit" :options="UNITS"
                optionLabel="l" optionValue="v" fluid
                @change="autoSave({ dashboard_unit: form.dashboard_unit })" />
      </div>
      <div>
        <label class="muted">Power LED</label>
        <Select v-model="form.power_led_mode" :options="POWER_LED_MODES"
                optionLabel="l" optionValue="v" fluid
                @change="autoSave({ power_led_mode: form.power_led_mode })" />
      </div>
      <div>
        <label class="muted">ECO LED</label>
        <Select v-model="form.eco_led_mode" :options="ECO_LED_MODES"
                optionLabel="l" optionValue="v" fluid
                @change="autoSave({ eco_led_mode: form.eco_led_mode })" />
      </div>
    </div>

    <div class="row" style="gap: 10px; align-items: flex-start;">
      <Checkbox v-model="restorePower" :binary="true" inputId="restore-power"
                @change="onRestorePowerChange" />
      <label for="restore-power" style="cursor: pointer; line-height: 1.4;">
        Remember on/off state across restarts
        <div class="muted" style="font-size: 12px; margin-top: 2px;">
          On (default): after a reboot or power cut the heater returns to
          whatever on/off state it was in. Off: it stays off until you press
          power.
        </div>
      </label>
    </div>

    <div style="border-top: 1px solid var(--border); padding-top: 16px; margin-top: 8px;">
      <h4 style="margin: 0 0 10px 0; font-size: 14px;">Password</h4>

      <div class="app-grid-3 app-grid-align-center">
        <Password v-model="oldPw" toggleMask :feedback="false"
                  placeholder="Current password" fluid />
        <Password v-model="newPw" toggleMask :feedback="false"
                  placeholder="New password" fluid />
        <Button label="Change password" severity="secondary" @click="changePassword" />
      </div>
      <p class="muted" style="margin: 6px 0 0 0; font-size: 12px;">
        You'll be signed out after the password is changed.
      </p>

      <div class="row" style="gap: 10px; align-items: flex-start; margin-top: 16px;">
        <Checkbox v-model="dashboardLocked" :binary="true" inputId="dash-lock"
                  @change="onDashboardLockChange" />
        <label for="dash-lock" style="cursor: pointer; line-height: 1.4;">
          Require password to view the dashboard
          <div class="muted" style="font-size: 12px; margin-top: 2px;">
            Off (default): anyone on the network can view temperatures and
            adjust controls. On: the dashboard also asks for the password.
          </div>
        </label>
      </div>
    </div>
  </div>
</template>

<style scoped>
.app-grid-3 {
  display: grid;
  grid-template-columns: 1fr 1fr 1fr;
  gap: 12px;
}
.app-grid-2 {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
  align-items: start;   /* inputs align at the top even when helper text differs in height */
}
.app-grid-align-center { align-items: center; }
@media (max-width: 520px) {
  .app-grid-3 { grid-template-columns: 1fr; }
  .app-grid-2 { grid-template-columns: 1fr; }
}
</style>
