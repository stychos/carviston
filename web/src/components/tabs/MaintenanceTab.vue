<script setup>
import { ref, computed, onMounted } from 'vue';
import Button from 'primevue/button';
import Dialog from 'primevue/dialog';
import FileUpload from 'primevue/fileupload';
import ProgressBar from 'primevue/progressbar';
import ProgressSpinner from 'primevue/progressspinner';
import { useConfirm } from 'primevue/useconfirm';
import { useToast } from 'primevue/usetoast';
import { api } from '../../composables/api.js';
import { token } from '../../composables/api.js';
import { state } from '../../composables/liveState.js';
import { safetyLabel } from '../../composables/safety.js';

const confirm = useConfirm();
const toast = useToast();

/* --- Update flow ---------------------------------------------------------
 * One blocking modal owns every restart-triggering action: OTA firmware
 * upload (the Vue UI is embedded in the image), plain reboot, factory reset,
 * boot-slot switch. The modal is non-dismissable (no close button, no escape, no
 * click-outside), so the user can't accidentally interact with anything
 * else while the device is busy.
 *
 * Phases:
 *   'uploading'  — XHR upload in flight; progress 0..100 %
 *   'writing'    — OTA only. Bytes are all sent; the device is verifying and
 *                  committing the new firmware (which carries the embedded UI)
 *                  and then reboots. Its single-task server is silent the
 *                  whole time, so we just poll /api/auth/status until it
 *                  answers again and reload then. Indeterminate — the upload
 *                  % is meaningless here, so no (frozen-looking) bar.
 *   'rebooting'  — a plain restart (reboot / factory-reset / boot-switch) is
 *                  in flight; waiting for the device to come back
 *   'reloading'  — device responded again; about to window.location.replace
 *   'error'      — upload/reboot failed; user can close to retry
 */
const otaDialog   = ref(false);
const otaPhase    = ref('idle');
const otaProgress = ref(0);
const otaError    = ref('');
const otaTitle    = ref('Updating');         /* "Updating", "Rebooting", … */
const rebootAttempt = ref(0);

const otaBusy = computed(() => otaDialog.value && otaPhase.value !== 'error');

function fetchWithTimeout(url, ms) {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), ms);
  return fetch(url, { cache: 'no-store', signal: ctrl.signal })
    .finally(() => clearTimeout(t));
}

/* Upper bound on how long we wait for the device to answer again. For a
 * web-bundle update we start polling while the device is still writing flash
 * (which can run well over a minute), so this needs generous headroom —
 * ~2.5 s per attempt. A plain reboot answers within the first few attempts. */
const REBOOT_POLL_MAX = 90;

/* Read /api/auth/status, returning the parsed body (or null if the device
 * isn't answering yet / the body wasn't JSON). */
async function fetchStatus(ms) {
  const r = await fetchWithTimeout('/api/auth/status', ms);
  if (!r.ok) return null;
  return await r.json().catch(() => ({}));
}

/* Poll until the device answers again. When `expectFwChange` is a previous
 * firmware id, only count it as "back" once the running firmware id DIFFERS —
 * so an aborted OTA that leaves the OLD image responsive is reported as a
 * failure instead of a false success (the poll alone can't tell a freshly
 * flashed device from one that never actually updated). */
async function waitForReboot(expectFwChange = null) {
  await new Promise(r => setTimeout(r, 3500));
  rebootAttempt.value = 0;
  for (let i = 0; i < REBOOT_POLL_MAX; i++) {
    rebootAttempt.value = i + 1;
    try {
      const s = await fetchStatus(1500);
      if (s) {
        if (!expectFwChange) return true;          /* plain reboot: any answer = back */
        if (s.fw && s.fw !== expectFwChange) return true;
        /* Answered, but still the old firmware id — the new image isn't
         * running. Keep polling; if it never changes we time out → error. */
      }
    } catch { /* device still down or fetch aborted */ }
    await new Promise(r => setTimeout(r, 1000));
  }
  return false;
}

/* Shared tail for every restart-triggering path: reload on return, else error. */
function resolveReturn(cameBack, failMsg) {
  if (cameBack) {
    otaPhase.value = 'reloading';
    setTimeout(manualReload, 200);
  } else {
    otaPhase.value = 'error';
    otaError.value = failMsg;
  }
}

/* Reload to the dashboard with a cache-buster so the freshly-flashed
 * web bundle isn't served from the browser cache. `replace` keeps the
 * post-flash URL out of the back-button history. */
function manualReload() {
  window.location.replace('/?v=' + Date.now());
}

/* Drive the modal through 'rebooting' → 'reloading' (or 'error'). Used by
 * every action that triggers an esp_restart on the backend. Called either
 * directly (reboot / factory-reset / boot-switch) or after an OTA upload's
 * 200 reply. */
async function runRebootFlow(title) {
  otaDialog.value = true;
  otaTitle.value  = title;
  otaPhase.value  = 'rebooting';
  rebootAttempt.value = 0;
  const came_back = await waitForReboot();
  resolveReturn(came_back, 'Device did not come back online — try refreshing the page manually.');
}

function dismissOtaDialog() {
  otaDialog.value  = false;
  otaPhase.value   = 'idle';
  otaError.value   = '';
  otaProgress.value = 0;
}

/* --- Boot partition info ------------------------------------------------- */
const boot = ref(null);
async function loadBoot() {
  try { boot.value = await api.get('/api/maintenance/boot'); }
  catch { boot.value = null; }
}
onMounted(loadBoot);

function slotName(slot) {
  if (slot === 0) return 'Firmware A';
  if (slot === 1) return 'Firmware B';
  return 'unknown';
}
const runningLabel  = computed(() => boot.value ? slotName(boot.value.running?.slot)   : '…');
const altLabel      = computed(() => boot.value ? slotName(boot.value.alternate?.slot) : '…');
const altBootable   = computed(() => !!boot.value?.alternate?.bootable);

const safetyText = computed(() => safetyLabel(state.value?.safety));
const safetyFaulted = computed(() => state.value && state.value.safety !== 0);

async function clearSafety() {
  try {
    state.value = await api.post('/api/safety/clear');
    toast.add({ severity: 'success', summary: 'Safety latch cleared',
                detail: 'Heaters were turned off — switch them back on if needed.',
                life: 4000 });
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}

function switchBoot() {
  if (!altBootable.value) return;
  confirm.require({
    message: `Restart and run ${altLabel.value}? If it doesn't start cleanly, the device will fall back to ${runningLabel.value} on the next boot — your settings are not affected.`,
    header: 'Switch firmware',
    icon: 'pi pi-sync',
    accept: async () => {
      try {
        await api.post('/api/maintenance/boot_switch');
        runRebootFlow(`Booting ${altLabel.value}`);
      } catch (e) {
        toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 4000 });
      }
    },
  });
}

async function uploadOta(event) {
  const file = event.files?.[0];
  if (!file) return;
  /* Modal opens immediately and locks the rest of the UI for the whole
   * upload→write→reboot→reload window. */
  otaDialog.value   = true;
  otaTitle.value    = 'Updating';
  otaPhase.value    = 'uploading';
  otaProgress.value = 0;
  otaError.value    = '';

  /* Capture the running firmware id BEFORE the upload starts so the
   * post-reboot poll can confirm the image actually changed. Awaited (not
   * fire-and-forget) so a fast upload can't finish before we have it; the
   * brief wait is invisible behind the 0 % progress bar. Best-effort: if it
   * can't be read we fall back to the plain "device answered" check. */
  let fwBefore = null;
  try { fwBefore = (await fetchStatus(2000))?.fw ?? null; } catch { /* keep null */ }

  /* We upload a firmware image (the Vue UI is embedded inside it). Two facts
   * about the device drive everything below:
   *
   *  1. xhr.upload.onprogress can reach 100 % a touch before the device has
   *     verified + committed the image — it counts bytes handed to the local
   *     socket, not work the device has finished — so a bar parked at 100 %
   *     can look frozen for the final verify/reboot.
   *  2. The device's success reply is routinely lost: it reboots within
   *     ~1.5 s of sending "200 rebooting", and the reply + final TCP ACKs
   *     can die with the connection. So we must NOT depend on xhr.onload,
   *     xhr.upload.onload, or xhr.onerror firing at all.
   *
   * What IS reliable: the device runs a SINGLE-TASK HTTP server, so while it
   * is verifying/installing and then rebooting it answers nothing. The instant
   * a GET of /api/auth/status succeeds again, it is back on the new firmware.
   * So the moment the body is on the wire we switch to the indeterminate
   * 'writing' phase and start polling for the device's return — the poll
   * self-times to however long the install+reboot takes, then reloads.
   *
   * We deliberately never offer a manual reload during 'writing': navigating
   * away aborts the XHR mid-upload and would corrupt the image. A poll only
   * succeeds once the server is back, which (being single-task) means the
   * upload handler has already returned. */
  let waitStarted = false;   /* entered 'writing' + began polling for return */
  let superseded  = false;   /* an explicit error response took over */

  async function startWriteWait() {
    if (waitStarted || superseded) return;
    waitStarted = true;
    if (otaPhase.value === 'uploading') otaPhase.value = 'writing';
    /* Require the firmware id to actually change: a dropped connection at
     * ~100 % aborts the flash and leaves the OLD image responsive, which the
     * old "did it answer?" check reported as success. */
    const came_back = await waitForReboot(fwBefore);
    if (superseded) return;             /* a 4xx/5xx response already errored out */
    resolveReturn(came_back,
      'The update did not take effect — the device is still running the previous '
      + 'firmware (the upload may have been interrupted). Please try again.');
  }

  function fail(msg) {
    superseded = true;
    otaPhase.value = 'error';
    otaError.value = msg;
  }

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Authorization', 'Bearer ' + token.value);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    otaProgress.value = Math.round((e.loaded / e.total) * 100);
    if (otaProgress.value >= 100) startWriteWait();
  };
  /* Belt-and-braces: some browsers skip the final 100 % progress tick. */
  xhr.upload.onload = () => startWriteWait();

  xhr.onload = () => {
    if (xhr.status === 200) {
      /* Device accepted the image and is rebooting — the poll (already
       * running, or started here) catches its return. */
      startWriteWait();
    } else {
      /* Explicit rejection (bad image, too large, …): the device is alive
       * and did NOT reboot. Supersede the poll so it can't see the live
       * device and wrongly reload. */
      fail(xhr.responseText || `Update failed (HTTP ${xhr.status}).`);
    }
  };
  xhr.onerror = () => {
    /* Once the body is fully buffered out we're in 'writing' and the poll
     * owns the outcome; a dropped socket here just means the device went
     * away to reboot. Only treat it as a failure if the upload never
     * finished. */
    if (!waitStarted && otaProgress.value < 100) {
      fail('Upload failed — check the connection to the device and try again.');
    }
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
        runRebootFlow('Rebooting');
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
        runRebootFlow('Resetting');
      } catch (e) {
        toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
      }
    },
  });
}
</script>

<template>
  <div class="stack">
    <!-- Safety latch: pinned to the top, only visible when there's
         actually something to clear. Compact row: pill + description on
         the left, Clear button right-aligned. -->
    <div v-if="safetyFaulted" class="tile maint-safety">
      <div class="maint-safety-row">
        <div class="maint-safety-text">
          <span class="pill bad"><span class="dot"></span>{{ safetyText }}</span>
          <span class="muted" style="font-size: 13px;">
            Heaters are locked off until you clear the latch. If the underlying
            problem is still present it will trip again on the next check.
          </span>
        </div>
        <Button label="Clear safety latch" icon="pi pi-shield"
                severity="warn" @click="clearSafety" />
      </div>
    </div>

    <div class="maint-row">
      <div class="tile">
        <h3>Reboot</h3>
        <span class="pill good" style="margin-bottom: 10px;">
          <span class="dot"></span>Running: {{ runningLabel }}
        </span>
        <p class="muted" style="margin: 0 0 10px 0; font-size: 13px;">
          Restarts the running firmware. Settings are preserved.
        </p>
        <Button label="Reboot" icon="pi pi-refresh" severity="secondary"
                @click="reboot" />
      </div>

      <div class="tile">
        <h3>Backup firmware</h3>
        <span class="pill" :class="altBootable ? '' : 'warn'" style="margin-bottom: 10px;">
          <span class="dot"></span>
          {{ altLabel }}
          <template v-if="boot"> — {{ altBootable ? 'ready' : 'empty' }}</template>
        </span>
        <p class="muted" style="margin: 0 0 10px 0; font-size: 13px;">
          Boots the backup slot. If it doesn't start, the device falls back
          to the running one automatically.
        </p>
        <Button :label="altBootable ? `Switch to ${altLabel}` : 'Backup is empty'"
                icon="pi pi-sync"
                :disabled="!altBootable" severity="secondary"
                @click="switchBoot" />
      </div>
    </div>

    <div class="maint-row">
      <div class="tile">
        <h3>Update</h3>
        <p class="muted" style="margin: 0 0 10px 0; font-size: 13px;">
          Upload a firmware <code>.bin</code> — it updates the dashboard too.
          The device verifies it and reboots automatically.
        </p>
        <FileUpload mode="basic" accept=".bin" :auto="true" customUpload
                    chooseLabel="Choose .bin" chooseIcon="pi pi-upload"
                    :disabled="otaBusy" @select="uploadOta" />
      </div>

      <div class="tile">
        <h3>Factory reset</h3>
        <p class="muted" style="margin: 0 0 10px 0; font-size: 13px;">
          Wipes the password and every setting. The device reboots into
          first-boot hotspot mode.
        </p>
        <Button label="Erase everything" icon="pi pi-trash"
                severity="danger" @click="factoryReset" />
      </div>
    </div>

    <!-- Unified blocking modal for uploads + restarts. The modal cannot be
         dismissed except in the error phase so the user cannot click any
         other control while an update / reboot is in flight. -->
    <Dialog v-model:visible="otaDialog" modal :draggable="false"
            :closable="otaPhase === 'error'"
            :dismissableMask="false" :closeOnEscape="otaPhase === 'error'"
            :header="otaPhase === 'error' ? 'Update failed' : otaTitle"
            :style="{ width: 'min(440px, 96vw)' }"
            @hide="otaPhase === 'error' && dismissOtaDialog()">
      <!-- UPLOADING — progress bar driven by xhr.upload.onprogress -->
      <div v-if="otaPhase === 'uploading'" class="reboot-modal">
        <ProgressBar :value="otaProgress" :showValue="false" style="width: 100%;" />
        <div class="reboot-label">Uploading…</div>
        <div class="muted" style="font-size: 13px; text-align: center;">
          Please don't close this tab. After the upload finishes, the
          device will write the image, reboot, and the page will reload
          to the dashboard.
        </div>
      </div>

      <!-- WRITING — body fully sent; device is verifying + committing the new
           firmware and rebooting. The upload % is meaningless now, so show an
           indeterminate spinner instead of a frozen bar. -->
      <div v-else-if="otaPhase === 'writing'" class="reboot-modal">
        <ProgressSpinner style="width: 56px; height: 56px;" strokeWidth="4" />
        <div class="reboot-label">Installing update…</div>
        <div class="muted" style="font-size: 13px; text-align: center;">
          Upload complete. The device is verifying the new firmware and will
          reboot on its own — this usually takes well under a minute. Please
          keep this tab open; the page reloads automatically once the device
          is back.
        </div>
      </div>

      <!-- REBOOTING — device-side write + restart + waitForReboot poll -->
      <div v-else-if="otaPhase === 'rebooting'" class="reboot-modal">
        <ProgressSpinner style="width: 56px; height: 56px;" strokeWidth="4" />
        <div class="reboot-label">Device is rebooting…</div>
        <div class="muted" style="font-size: 13px; text-align: center;">
          Waiting for the device to come back online. The page will reload
          automatically when it does.
          <template v-if="rebootAttempt > 0">
            <br />Attempt {{ rebootAttempt }} of {{ REBOOT_POLL_MAX }}…
          </template>
        </div>
        <Button v-if="rebootAttempt >= 5"
                label="Reload now" icon="pi pi-refresh"
                severity="secondary" size="small"
                @click="manualReload" />
      </div>

      <!-- RELOADING — device responded; about to window.location.replace -->
      <div v-else-if="otaPhase === 'reloading'" class="reboot-modal">
        <ProgressSpinner style="width: 56px; height: 56px;" strokeWidth="4" />
        <div class="reboot-label">Loading the dashboard…</div>
      </div>

      <!-- ERROR — upload failed or device didn't come back; user can close -->
      <div v-else-if="otaPhase === 'error'" class="reboot-modal">
        <i class="pi pi-exclamation-triangle"
           style="font-size: 48px; color: var(--bad);"></i>
        <div class="reboot-label" style="text-align: center;">{{ otaError }}</div>
        <Button label="Close" severity="secondary" @click="dismissOtaDialog" />
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

/* Pairs of cards (Reboot+Backup, Update+Factory reset) sit in two equal
 * columns; both stack on narrow screens. Items flex-column so the action
 * controls line up horizontally regardless of paragraph length. */
.maint-row {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}
.maint-row > .tile {
  display: flex; flex-direction: column;
  justify-content: space-between;
}
@media (max-width: 520px) {
  .maint-row { grid-template-columns: 1fr; }
}

/* Safety-issue card stands out — red left accent so the user can't miss
 * it when it appears at the top. Compact row: pill + description take the
 * available width, button right-aligned on its own. */
.maint-safety {
  border-left: 4px solid var(--bad);
}
.maint-safety-row {
  display: flex; align-items: center; gap: 14px;
  flex-wrap: wrap;
}
.maint-safety-text {
  flex: 1; min-width: 0;
  display: flex; flex-wrap: wrap; align-items: center; gap: 8px;
}
</style>
