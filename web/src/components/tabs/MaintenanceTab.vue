<script setup>
import { ref, computed, onMounted } from 'vue';
import Button from 'primevue/button';
import Checkbox from 'primevue/checkbox';
import Dialog from 'primevue/dialog';
import FileUpload from 'primevue/fileupload';
import Password from 'primevue/password';
import ProgressBar from 'primevue/progressbar';
import ProgressSpinner from 'primevue/progressspinner';
import { useConfirm } from 'primevue/useconfirm';
import { useToast } from 'primevue/usetoast';
import { api } from '../../composables/api.js';
import { token } from '../../composables/api.js';
import { refreshStatus } from '../../composables/auth.js';

/* --- Reboot-and-redirect helper ----------------------------------------- *
 * Used by every action that triggers an esp_restart() on the backend (OTA,
 * web bundle, plain reboot, factory reset, boot-slot switch). Polls the
 * device until it answers /api/auth/status again, then navigates to the
 * landing page so the user gets a freshly loaded SPA (and the new web
 * bundle when one was flashed). The non-dismissable dialog is the only
 * UI shown during this window. */
const rebooting = ref(false);
const rebootingLabel = ref('Rebooting…');

async function waitForReboot() {
  /* Initial grace period — esp_restart fires ~800 ms after the success
   * reply; the bootloader + app come up over the next few seconds. */
  await new Promise(r => setTimeout(r, 4000));
  /* Then poll for up to ~90 seconds (60 attempts × 1.5 s). */
  for (let i = 0; i < 60; i++) {
    try {
      const r = await fetch('/api/auth/status', { cache: 'no-store' });
      if (r.ok) return true;
    } catch { /* device still down */ }
    await new Promise(r => setTimeout(r, 1500));
  }
  return false;
}

async function rebootAndRedirect(label) {
  rebootingLabel.value = label;
  rebooting.value = true;
  const came_back = await waitForReboot();
  if (came_back) {
    /* href (not reload) plus a cache-buster so an OTA-ed web bundle isn't
     * served from the browser's cache. App.vue refreshes /api/auth/status
     * on load, so any token expiry / configured-flag flip from a factory
     * reset is observed cleanly. */
    window.location.href = '/?v=' + Date.now();
  } else {
    rebooting.value = false;
    toast.add({
      severity: 'warn',
      summary: 'Device did not come back online',
      detail: 'Check power and reload manually.',
      life: 6000,
    });
  }
}

const confirm = useConfirm();
const toast = useToast();

const otaProgress = ref(0);
const otaBusy = ref(false);

/* --- Boot partition info ------------------------------------------------- */
const boot = ref(null);
async function loadBoot() {
  try { boot.value = await api.get('/api/maintenance/boot'); }
  catch { boot.value = null; }
}
onMounted(loadBoot);

function slotName(slot) {
  if (slot === 0 || slot === 1) return `OTA ${slot}`;
  return 'unknown';
}
const runningLabel  = computed(() => boot.value ? slotName(boot.value.running?.slot)   : '…');
const altLabel      = computed(() => boot.value ? slotName(boot.value.alternate?.slot) : '…');
const altBootable   = computed(() => !!boot.value?.alternate?.bootable);

/* --- Dashboard lock ------------------------------------------------------ */
/* The checkbox writes to the bound ref synchronously; we react on @change
 * so the initial load (which just sets the ref) doesn't trigger a save. */
const dashboardLocked = ref(false);
async function loadDashboardLock() {
  try {
    const cfg = await api.get('/api/config');
    dashboardLocked.value = !!cfg.dashboard_locked;
  } catch { /* leave default */ }
}
onMounted(loadDashboardLock);
async function onDashboardLockChange(e) {
  const v = !!e.checked;
  try {
    await api.put('/api/config', { dashboard_locked: v });
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

const oldPw = ref('');
const newPw = ref('');
async function changePassword() {
  if (!newPw.value || newPw.value.length < 4) {
    toast.add({ severity: 'warn', summary: 'New password too short', life: 2500 });
    return;
  }
  try {
    await api.post('/api/auth/password', { old: oldPw.value, new: newPw.value });
    toast.add({ severity: 'success', summary: 'Password updated — sign in again', life: 3000 });
    oldPw.value = ''; newPw.value = '';
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}

function switchBoot() {
  if (!altBootable.value) return;
  confirm.require({
    message: `Reboot into ${altLabel.value}? If the alternate slot fails to start, the device will auto-rollback to ${runningLabel.value} on next boot.`,
    header: 'Switch boot slot',
    icon: 'pi pi-sync',
    accept: async () => {
      try {
        await api.post('/api/maintenance/boot_switch');
        rebootAndRedirect(`Booting ${altLabel.value}…`);
      } catch (e) {
        toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 4000 });
      }
    },
  });
}

function uploadOta(event) {
  const file = event.files?.[0];
  if (!file) return;
  otaBusy.value = true;
  otaProgress.value = 0;
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Authorization', 'Bearer ' + token.value);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) otaProgress.value = Math.round((e.loaded / e.total) * 100);
  };
  xhr.onload = () => {
    otaBusy.value = false;
    if (xhr.status === 200) {
      rebootAndRedirect('Flashing complete — device is rebooting');
    } else {
      toast.add({ severity: 'error', summary: 'OTA failed', detail: xhr.responseText, life: 4000 });
    }
  };
  xhr.onerror = () => {
    otaBusy.value = false;
    toast.add({ severity: 'error', summary: 'OTA upload error', life: 4000 });
  };
  xhr.send(file);
}

function reboot() {
  confirm.require({
    message: 'Reboot the controller now? Heaters will shut off during boot.',
    header: 'Reboot',
    icon: 'pi pi-refresh',
    accept: async () => {
      try {
        await api.post('/api/maintenance/reboot');
        rebootAndRedirect('Rebooting…');
      } catch (e) {
        toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
      }
    },
  });
}

function factoryReset() {
  confirm.require({
    message: 'Hardware reset wipes the password, all settings, and Wi-Fi credentials. The device will reboot into AP mode.',
    header: 'Hardware reset',
    icon: 'pi pi-exclamation-triangle',
    acceptClass: 'p-button-danger',
    acceptLabel: 'Wipe and reboot',
    accept: async () => {
      try {
        await api.post('/api/maintenance/factory_reset');
        rebootAndRedirect('Resetting and rebooting into AP mode');
      } catch (e) {
        toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
      }
    },
  });
}
</script>

<template>
  <div class="stack">
    <div class="tile">
      <h3>Reboot</h3>
      <p class="muted">Restarts the controller. Settings are preserved.</p>
      <Button label="Reboot" icon="pi pi-refresh" severity="secondary" @click="reboot" />
    </div>

    <div class="tile">
      <h3>Boot partition</h3>
      <div class="row" style="flex-wrap: wrap; gap: 8px;">
        <span class="pill good"><span class="dot"></span>Running: {{ runningLabel }}</span>
        <span class="pill" :class="altBootable ? '' : 'warn'">
          <span class="dot"></span>
          Alternate: {{ altLabel }}
          <template v-if="boot"> — {{ altBootable ? 'valid' : 'empty' }}</template>
        </span>
      </div>
      <div style="margin-top: 10px;">
        <Button :label="`Boot from ${altLabel}`" icon="pi pi-sync"
                :disabled="!altBootable" severity="secondary"
                @click="switchBoot" />
      </div>
      <p class="muted" style="margin: 10px 0 0 0; font-size: 12px;">
        Switches the active firmware slot and reboots. Use after flashing a
        new firmware to the other slot when you want to try it without
        overwriting the current one.
      </p>
    </div>

    <div class="tile">
      <h3>OTA update</h3>
      <p class="muted">
        Upload firmware or a web bundle (<code>.bin</code>). The device
        recognises the file type automatically and reboots when done.
      </p>
      <FileUpload mode="basic" accept=".bin" :auto="true" customUpload chooseLabel="Choose .bin"
                  :disabled="otaBusy" @select="uploadOta" />
      <ProgressBar v-if="otaBusy" :value="otaProgress" style="margin-top: 10px;" />
    </div>

    <div class="tile">
      <h3>Password</h3>

      <div class="row" style="gap: 10px; align-items: flex-start;">
        <Checkbox v-model="dashboardLocked" :binary="true" inputId="dash-lock"
                  @change="onDashboardLockChange" />
        <label for="dash-lock" style="cursor: pointer; line-height: 1.4;">
          Require password to view the dashboard
          <div class="muted" style="font-size: 12px; margin-top: 2px;">
            Off (default): anyone on the network can view temperatures and
            adjust target/mode/power. On: the dashboard also asks for the
            password. Settings are always password-protected.
          </div>
        </label>
      </div>

      <hr style="border: 0; border-top: 1px solid var(--border); margin: 16px 0;" />

      <p class="muted" style="margin: 0 0 8px 0;">
        Update the admin password. You'll be signed out after the change.
      </p>
      <div class="stack" style="gap: 8px;">
        <Password v-model="oldPw" toggleMask :feedback="false" placeholder="Current password" fluid />
        <Password v-model="newPw" toggleMask placeholder="New password" fluid />
        <Button label="Change password" severity="secondary" @click="changePassword" />
      </div>
    </div>

    <div class="tile">
      <h3>Hardware reset</h3>
      <p class="muted">Wipes password and all settings. The device reboots into first-boot AP mode.</p>
      <Button label="Hardware reset" icon="pi pi-trash" severity="danger" @click="factoryReset" />
    </div>

    <Dialog v-model:visible="rebooting" modal :draggable="false"
            :closable="false" :dismissableMask="false" :closeOnEscape="false"
            header="Please wait"
            :style="{ width: 'min(420px, 96vw)' }">
      <div class="reboot-modal">
        <ProgressSpinner style="width: 56px; height: 56px;" strokeWidth="4" />
        <div class="reboot-label">{{ rebootingLabel }}</div>
        <div class="muted" style="font-size: 13px; text-align: center;">
          Waiting for the device to come back online. The page will reload
          automatically when it does.
        </div>
      </div>
    </Dialog>
  </div>
</template>

<style scoped>
.reboot-modal {
  display: flex; flex-direction: column; align-items: center; gap: 14px;
  padding: 8px 4px 4px;
}
.reboot-label { font-size: 15px; font-weight: 500; }
</style>
