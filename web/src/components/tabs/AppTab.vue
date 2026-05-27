<script setup>
import { reactive, ref, watch, onMounted } from 'vue';
import Select from 'primevue/select';
import InputText from 'primevue/inputtext';
import Password from 'primevue/password';
import Button from 'primevue/button';
import { useToast } from 'primevue/usetoast';
import { api, HttpError } from '../../composables/api.js';

const props = defineProps({
  cfg: Object,
  onSave: { type: Function, required: true },
});
const toast = useToast();

const WIFI_MODES = [
  { v: 0, l: 'Access Point only' },
  { v: 1, l: 'Client (STA)' },
  { v: 2, l: 'Hybrid (AP + STA simultaneously)' },
];

const form = reactive({
  wifi_mode: props.cfg.wifi_mode,
  sta_ssid:  props.cfg.sta_ssid,
  sta_pass:  '',
  ap_ssid:   props.cfg.ap_ssid,
  ap_pass:   '',
});
watch(() => props.cfg, c => Object.assign(form, {
  wifi_mode: c.wifi_mode, sta_ssid: c.sta_ssid, ap_ssid: c.ap_ssid,
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
      toast.add({ severity: 'success', summary: 'Connected — credentials work',
                  detail: 'Save to make this persistent', life: 4000 });
    } else {
      toast.add({ severity: 'error', summary: 'Could not connect',
                  detail: r.error || 'unknown reason', life: 4000 });
    }
  } catch (e) {
    const detail = (e instanceof HttpError) ? e.message : (e?.message || String(e));
    toast.add({ severity: 'error', summary: 'Test failed', detail, life: 4000 });
  } finally {
    testing.value = false;
  }
}

async function saveNetwork() {
  const patch = {
    wifi_mode: form.wifi_mode,
    sta_ssid: form.sta_ssid,
    ap_ssid: form.ap_ssid,
  };
  if (form.sta_pass) patch.sta_pass = form.sta_pass;
  if (form.ap_pass)  patch.ap_pass  = form.ap_pass;
  try {
    await props.onSave(patch);
    toast.add({ severity: 'success', summary: 'Saved', life: 2500 });
    form.sta_pass = ''; form.ap_pass = '';
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}
</script>

<template>
  <div class="stack">
    <label class="muted">Wi-Fi mode</label>
    <Select v-model="form.wifi_mode" :options="WIFI_MODES" optionLabel="l" optionValue="v" fluid />

    <div v-if="form.wifi_mode !== 0">
      <div class="spaced">
        <label class="muted">Available networks</label>
        <Button label="Rescan" size="small" text :loading="scanning" @click="scan" />
      </div>
      <div style="max-height: 140px; overflow: auto; border: 1px solid var(--border); border-radius: 8px; padding: 4px;">
        <div v-for="n in networks" :key="n.ssid + n.channel"
             style="padding: 6px 8px; cursor: pointer; border-radius: 6px;"
             @click="form.sta_ssid = n.ssid"
             :style="{ background: form.sta_ssid === n.ssid ? 'var(--surface-2)' : '' }">
          <span>{{ n.ssid }}</span>
          <span class="muted" style="float: right; font-size: 12px;">{{ n.rssi }} dBm</span>
        </div>
        <div v-if="!networks.length" class="muted" style="padding: 8px;">No networks yet.</div>
      </div>
      <label class="muted" style="margin-top: 8px; display: block;">STA SSID</label>
      <InputText v-model="form.sta_ssid" fluid />
      <label class="muted">STA password</label>
      <Password v-model="form.sta_pass" toggleMask :feedback="false" placeholder="leave blank to keep current" fluid />

      <Button label="Test connection" icon="pi pi-bolt"
              severity="secondary" :loading="testing"
              :disabled="!form.sta_ssid" style="margin-top: 8px;"
              @click="testCredentials" />
      <p class="muted" style="margin: 6px 0 0 0; font-size: 12px;">
        Tries the credentials without saving. The AP stays up during the
        check, so you won't be kicked off.
      </p>
    </div>

    <div v-if="form.wifi_mode === 0 || form.wifi_mode === 2">
      <label class="muted">AP SSID (blank = auto carviston-XXXXXX)</label>
      <InputText v-model="form.ap_ssid" fluid />
      <label class="muted">AP password (blank = open)</label>
      <Password v-model="form.ap_pass" toggleMask :feedback="false" placeholder="leave blank to keep / be open" fluid />
    </div>

    <Button label="Save network settings" @click="saveNetwork" />
  </div>
</template>
