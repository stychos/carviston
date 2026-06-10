<script setup>
import { ref, computed, onMounted } from 'vue';
import Button from 'primevue/button';
import Checkbox from 'primevue/checkbox';
import Dialog from 'primevue/dialog';
import FileUpload from 'primevue/fileupload';
import ProgressBar from 'primevue/progressbar';
import ProgressSpinner from 'primevue/progressspinner';
import { useConfirm } from 'primevue/useconfirm';
import { useToast } from 'primevue/usetoast';
import { api } from '../../composables/api.js';
import { token } from '../../composables/api.js';
import { state, disconnectLive, connectLive } from '../../composables/liveState.js';
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
const otaUpload   = ref(null);               /* FileUpload instance, for clear() */
const otaConfirm  = ref(false);              /* pre-flash confirm dialog visible */
const otaFile     = ref(null);               /* the chosen .bin awaiting confirmation */
const otaResetCfg = ref(false);              /* checkbox: reset settings on flash */
const otaFileLabel = computed(() => {
  const f = otaFile.value;
  return f ? `${f.name} · ${(f.size / 1048576).toFixed(2)} MB` : '';
});
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

/* Upper bound on poll iterations while waiting for the device to come back.
 * We start polling while the device is still writing flash (which can run well
 * over a minute), so this needs generous headroom — up to ~2.2 s per iteration
 * (1.5 s fetch timeout + 0.7 s gap). A plain reboot returns within a few. */
const REBOOT_POLL_MAX = 90;

/* Poll /api/auth/status until we can prove the device rebooted onto the
 * expected image. Two independent success signals, in priority order:
 *
 *  1. fw CHANGED (OTA only) — /api/auth/status carries a short id of the running
 *     firmware (running_fw_id() in web_server.c, built for exactly this). When
 *     the caller hands us the pre-upload `baselineFw`, a reply whose `fw` DIFFERS
 *     is unambiguous proof the NEW image is up. This is a positive LEVEL test, so
 *     it works even if we start polling after the device is already back — which
 *     is the case the OTA flow routinely hits: the success reply is often lost
 *     and esp_restart can drop the socket silently, so polling may not begin
 *     until the device has finished a (fast) reboot. Relying only on signal 2
 *     below missed that window and stranded the dialog.
 *
 *  2. offline → online edge — for same-fw restarts (plain reboot, factory reset)
 *     there is no fw change to see, so we fall back to "went unreachable, then
 *     answered again". Requiring the transition (not merely "it answered") is
 *     what tells a real reboot apart from an upload that aborted with the OLD
 *     image still serving: there the device neither drops offline NOR changes fw,
 *     so neither signal fires, we time out, and report failure — as intended. */
async function waitForReboot(baselineFw) {
  rebootAttempt.value = 0;
  let sawDown = false;
  for (let i = 0; i < REBOOT_POLL_MAX; i++) {
    rebootAttempt.value = i + 1;
    let up = false;
    let fw = null;
    try {
      const r = await fetchWithTimeout('/api/auth/status', 1500);
      up = r.ok;
      if (up) fw = (await r.json().catch(() => null))?.fw ?? null;
    } catch { up = false; /* device still down or fetch aborted */ }
    /* New firmware confirmed — done regardless of whether we ever saw it drop. */
    if (up && baselineFw && fw && fw !== baselineFw) return true;
    if (!up)          sawDown = true;   /* offline — still writing / rebooting */
    else if (sawDown) return true;      /* offline → online: it came back (same fw) */
    await new Promise(r => setTimeout(r, 700));
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
    /* We tore down live polling for the duration of the restart flow; the page
     * won't reload now, so bring it back so the dashboard behind the error
     * dialog keeps updating (idempotent — connectLive() no-ops if already up). */
    connectLive();
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
  /* Stop background live-polling before we start watching for the return:
   * liveState's /api/state GETs are un-abortable (no timeout) and, against the
   * single-task server that goes dark across a restart, hang and tie up the
   * browser's small per-host connection pool — which can starve waitForReboot's
   * own probe. The post-return reload re-runs connectLive(); the error path
   * (resolveReturn) brings it back if we don't reload. */
  disconnectLive();
  otaDialog.value = true;
  otaTitle.value  = title;
  otaPhase.value  = 'rebooting';
  rebootAttempt.value = 0;
  /* Same-fw restart (reboot / factory reset / boot switch): no baseline fw, so
   * detection rides the offline→online edge. */
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

/* Picking a file no longer flashes immediately — flashing reboots the heater
 * and is not freely reversible, so confirm intent first (and offer an opt-in
 * config reset). On accept we hand the captured file to startUpload(); either
 * way we clear the picker so the same file can be re-chosen after a cancel. */
function uploadOta(event) {
  const file = event.files?.[0];
  otaUpload.value?.clear?.();
  if (!file) return;
  otaFile.value     = file;
  otaResetCfg.value = false;        /* default: keep current configuration */
  otaConfirm.value  = true;
}

function confirmFlash() {
  otaConfirm.value = false;
  const file = otaFile.value;
  if (file) startUpload(file, otaResetCfg.value);
}

async function startUpload(file, resetConfig) {
  if (!file) return;
  /* Stop background live-polling for the whole upload→reboot→reload window.
   * liveState's /api/state GETs are un-abortable (no timeout) and, against the
   * single-task server that is busy streaming the image and then dark across the
   * reboot, they hang and tie up the browser's small per-host connection pool —
   * starving waitForReboot's readiness probe. The post-flash reload re-runs
   * connectLive(); the failure paths (fail / resolveReturn) bring it back. */
  disconnectLive();
  /* Modal opens immediately and locks the rest of the UI for the whole
   * upload→write→reboot→reload window. */
  otaDialog.value   = true;
  otaTitle.value    = 'Updating';
  otaPhase.value    = 'uploading';
  otaProgress.value = 0;
  otaError.value    = '';
  rebootAttempt.value = 0;

  /* Snapshot the running firmware id NOW, while the device is healthy, so the
   * readiness poll can positively confirm the NEW image booted (a CHANGED fw),
   * instead of depending on catching the device offline. Best-effort: if it
   * fails, baselineFw stays null and waitForReboot falls back to the
   * offline→online edge. */
  let baselineFw = null;
  try {
    const s = await fetchWithTimeout('/api/auth/status', 1500);
    if (s.ok) baselineFw = (await s.json().catch(() => null))?.fw ?? null;
  } catch { /* no baseline — fall back to reachability edge */ }

  /* We upload a firmware image (the Vue UI is embedded inside it). The flow
   * mirrors the plain-reboot path's robustness rule: only start polling for the
   * device's return AFTER the trigger request's connection has closed — never
   * while it is still open.
   *
   *  1. xhr.upload.onprogress reaching 100 % means the body is on the local
   *     socket, NOT that the device has finished verifying/committing. At that
   *     point we switch to the indeterminate 'writing' phase (spinner) but do
   *     NOT yet poll — the upload connection is still live, and polling against
   *     a live-then-orphaned connection poisons the browser's pool: a lost
   *     reply leaves it half-open and subsequent polls get reused onto the dead
   *     socket, so the device never reads as "back" even once it is.
   *  2. We begin the reboot poll only once the XHR terminally settles:
   *       - onload 200  → device committed the image and is rebooting;
   *       - onerror     → the socket dropped (esp_restart RSTs it), reply lost;
   *       - onabort     → defensive, treated like onerror.
   *     In every case the upload connection is now closed and out of the reuse
   *     pool, so the polls open clean sockets — exactly like the reboot flow,
   *     which polls only after its short POST has settled.
   *  3. Fallback: if the socket drops SILENTLY (no RST, so neither onload nor
   *     onerror fires promptly), start polling anyway after a grace period so a
   *     stuck settle can't strand the dialog. This is the only path that may
   *     poll against a still-pooled connection, and it is rare.
   *
   * Starting the poll late is now SAFE even when the device has already finished
   * its (fast) reboot by the time we begin: waitForReboot confirms the return by
   * a CHANGED firmware id (baselineFw → new fw), a positive level test that does
   * not require having observed the device offline first. That is what fixes the
   * silent-drop case (3) where polling could begin only after the device was back
   * — the old reachability-only check needed an offline→online edge it never saw,
   * and stranded the dialog.
   *
   * A 200 that arrives while the device is still serving the OLD firmware (its
   * ~1.2 s pre-reboot delay) is harmless: the poll sees the unchanged fw, holds,
   * and only returns once the fw flips to the new image (or the offline→online
   * edge fires).
   *
   * We never offer a manual reload before the poll starts: until the XHR has
   * settled, navigating away could abort the upload mid-flight. */
  let enteredWriting = false;   /* body fully sent; phase switched to 'writing' */
  let waitStarted    = false;   /* reboot poll has begun */
  let superseded     = false;   /* an explicit error response took over */
  let fallbackTimer  = null;    /* silent-drop guard; armed on entering 'writing' */

  /* Body is on the wire — show the indeterminate 'writing' spinner and arm the
   * silent-drop fallback, but do NOT poll yet (the upload connection is live). */
  function enterWriting() {
    if (enteredWriting || superseded) return;
    enteredWriting = true;
    if (otaPhase.value === 'uploading') otaPhase.value = 'writing';
    fallbackTimer = setTimeout(startWriteWait, 8000);
  }

  /* Begin polling for the device's return — only ever after the upload XHR has
   * settled (or the silent-drop fallback fires), so the polls run on clean
   * connections. */
  async function startWriteWait() {
    if (waitStarted || superseded) return;
    waitStarted = true;
    if (fallbackTimer) { clearTimeout(fallbackTimer); fallbackTimer = null; }
    if (otaPhase.value !== 'writing') otaPhase.value = 'writing';
    const came_back = await waitForReboot(baselineFw);
    if (superseded) return;             /* a 4xx/5xx response already errored out */
    resolveReturn(came_back,
      'The device did not come back online after the update — it may have been '
      + 'interrupted. Refresh the page to check, or try again.');
  }

  function fail(msg) {
    superseded = true;
    if (fallbackTimer) { clearTimeout(fallbackTimer); fallbackTimer = null; }
    otaPhase.value = 'error';
    otaError.value = msg;
    /* Upload rejected/aborted: the device is alive and did NOT reboot. We won't
     * reload, so resume the live polling we tore down at the start. */
    connectLive();
  }

  const xhr = new XMLHttpRequest();
  xhr.open('POST', resetConfig ? '/api/ota?reset_config=1' : '/api/ota');
  xhr.setRequestHeader('Authorization', 'Bearer ' + token.value);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    otaProgress.value = Math.round((e.loaded / e.total) * 100);
    if (otaProgress.value >= 100) enterWriting();
  };
  /* Belt-and-braces: some browsers skip the final 100 % progress tick. */
  xhr.upload.onload = () => enterWriting();

  xhr.onload = () => {
    if (xhr.status === 200) {
      /* Device accepted the image, committed it, and is rebooting. The reply
       * arrived, so this connection is closed — poll for the device's return
       * on a clean socket. */
      startWriteWait();
    } else {
      /* Explicit rejection (bad image, too large, …): the device is alive
       * and did NOT reboot. Supersede the poll so it can't see the live
       * device and wrongly reload. */
      fail(xhr.responseText || `Update failed (HTTP ${xhr.status}).`);
    }
  };
  xhr.onerror = () => {
    /* A dropped socket BEFORE the body is fully sent is a genuine upload
     * failure. After that (we're in 'writing'), the drop just means the device
     * went away to reboot — its connection is now dead and out of the reuse
     * pool, so start polling on a fresh socket. */
    if (!enteredWriting && otaProgress.value < 100) {
      fail('Upload failed — check the connection to the device and try again.');
    } else {
      startWriteWait();
    }
  };
  xhr.onabort = () => { if (enteredWriting) startWriteWait(); };
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
          Upload a new firmware.
          The device verifies it and reboots automatically.
        </p>
        <FileUpload ref="otaUpload" mode="basic" accept=".bin" :auto="true" customUpload
                    chooseLabel="Choose Firmware" chooseIcon="pi pi-upload"
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

    <!-- Pre-flash confirmation. Hosts the reset-settings opt-in, which a plain
         confirm() can't, so it's a dedicated dialog. -->
    <Dialog v-model:visible="otaConfirm" modal :draggable="false"
            :style="{ width: '26rem' }" header="Flash firmware">
      <div class="flash-confirm">
        <p style="margin: 0 0 4px;">Flash this image now?</p>
        <p class="muted firmware-name">{{ otaFileLabel }}</p>
        <p class="muted" style="font-size: 13px; line-height: 1.45;">
          The controller writes the new firmware and reboots — the heaters shut
          off during the update. Keep this tab open until it finishes.
        </p>
        <label class="row reset-opt">
          <Checkbox v-model="otaResetCfg" :binary="true" inputId="ota-reset" />
          <span>
            Reset all settings to defaults
            <span class="muted" style="display: block; font-size: 12px;">
              Wi-Fi and password are kept. Leave unchecked to preserve your
              current configuration.
            </span>
          </span>
        </label>
      </div>
      <template #footer>
        <Button label="Cancel" text @click="otaConfirm = false" />
        <Button :label="otaResetCfg ? 'Flash & reset settings' : 'Upload and flash'"
                icon="pi pi-upload"
                :severity="otaResetCfg ? 'danger' : 'primary'" @click="confirmFlash" />
      </template>
    </Dialog>

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
          <template v-if="rebootAttempt > 0">
            <br /><br />Checking if it's back… attempt {{ rebootAttempt }} of {{ REBOOT_POLL_MAX }}.
          </template>
        </div>
        <Button v-if="rebootAttempt >= 5"
                label="Reload now" icon="pi pi-refresh"
                severity="secondary" size="small"
                @click="manualReload" />
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

.flash-confirm { display: flex; flex-direction: column; gap: 6px; }
.flash-confirm .firmware-name { font-weight: 600; word-break: break-all; margin: 0; }
.reset-opt {
  align-items: flex-start; gap: 10px; margin-top: 8px;
  padding: 10px; border: 1px solid var(--border); border-radius: 8px;
  cursor: pointer;
}

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
