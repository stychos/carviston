<script setup>
import { ref, watch, computed } from 'vue';
import Button from 'primevue/button';
import Checkbox from 'primevue/checkbox';
import Select from 'primevue/select';
import { useToast } from 'primevue/usetoast';
import { state as liveState } from '../../composables/liveState.js';
import { api } from '../../composables/api.js';

const props = defineProps({
  cfg: Object,
  onSave: { type: Function, required: true },
});
const toast = useToast();

const SCHED_MAX = 16;

const pad = n => String(n).padStart(2, '0');

/* The device reasons about schedule times in LOCAL wall-clock. In STA mode that
 * comes from SNTP; in AP mode (no internet) the user seeds it by hand. Either
 * way the firmware refuses to fire until the clock is usable, so we show the
 * device's own clock and lock the editor until it is. */
const deviceTime = computed(() => liveState.value?.device_time || null);
const clockReady = computed(() => liveState.value?.time_synced === true);
const clockManual = computed(() => liveState.value?.time_manual === true);

/* AP mode (WIFI_ROLE_AP = 0) can't sync at all, so it always gets the manual
 * set-the-clock control instead of the timezone selector. STA keeps the
 * selector + SNTP path, but ALSO offers manual set whenever it hasn't actually
 * synced (no internet) or is currently running a manual clock — so an offline
 * STA can still drive the scheduler until NTP comes through. */
const isAP = computed(() => props.cfg.wifi_mode === 0);
const showManual = computed(() => isAP.value || !clockReady.value || clockManual.value);

/* datetime-local <input> bound value, prefilled with the browser's wall-clock. */
function nowLocalInput() {
  const d = new Date();
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}` +
         `T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}
const manualTime = ref(nowLocalInput());

async function setManualTime() {
  const d = new Date(manualTime.value);
  if (isNaN(d.getTime())) {
    toast.add({ severity: 'warn', summary: 'Enter a valid date & time', life: 2500 });
    return;
  }
  /* Send the entered LOCAL components reinterpreted as UTC0: the firmware runs
   * its clock in UTC0 so localtime reads back exactly these values. */
  const epoch = Math.floor(Date.UTC(
    d.getFullYear(), d.getMonth(), d.getDate(),
    d.getHours(), d.getMinutes(), d.getSeconds()) / 1000);
  try {
    await api.post('/api/time', { epoch });
    toast.add({ severity: 'success', summary: 'Device time set', life: 2000 });
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}

/* Curated POSIX TZ strings (DST rules baked in where the zone observes it). The
 * value is what the firmware feeds setenv("TZ"). */
const ZONES = [
  { label: 'UTC', value: 'UTC0' },
  { label: 'UK / Ireland / Portugal (GMT/BST)', value: 'GMT0BST,M3.5.0/1,M10.5.0' },
  { label: 'Central Europe — Berlin/Paris/Madrid (CET/CEST)', value: 'CET-1CEST,M3.5.0,M10.5.0/3' },
  { label: 'Eastern Europe — Athens/Helsinki (EET/EEST)', value: 'EET-2EEST,M3.5.0/3,M10.5.0/4' },
  { label: 'Moscow / Istanbul (+3, no DST)', value: '<+03>-3' },
  { label: 'Gulf (+4, no DST)', value: '<+04>-4' },
  { label: 'India (+5:30)', value: 'IST-5:30' },
  { label: 'US Eastern', value: 'EST5EDT,M3.2.0,M11.1.0' },
  { label: 'US Central', value: 'CST6CDT,M3.2.0,M11.1.0' },
  { label: 'US Mountain', value: 'MST7MDT,M3.2.0,M11.1.0' },
  { label: 'US Arizona (no DST)', value: 'MST7' },
  { label: 'US Pacific', value: 'PST8PDT,M3.2.0,M11.1.0' },
];
const tz = ref(props.cfg.timezone || 'UTC0');

/* Weekday bits match the firmware (struct tm.tm_wday): bit0=Sun … bit6=Sat.
 * Shown Mon-first since that's the common European week start. */
const DAYS = [
  { label: 'M', title: 'Monday',    bit: 1 << 1 },
  { label: 'T', title: 'Tuesday',   bit: 1 << 2 },
  { label: 'W', title: 'Wednesday', bit: 1 << 3 },
  { label: 'T', title: 'Thursday',  bit: 1 << 4 },
  { label: 'F', title: 'Friday',    bit: 1 << 5 },
  { label: 'S', title: 'Saturday',  bit: 1 << 6 },
  { label: 'S', title: 'Sunday',    bit: 1 << 0 },
];
const EVERY = 0x7F, WEEKDAYS = 0x3E, WEEKENDS = 0x41;

const ACTIONS = [
  { label: 'On',  value: 1 },
  { label: 'Off', value: 0 },
];

function fromCfg() {
  const list = Array.isArray(props.cfg.schedule) ? props.cfg.schedule : [];
  return {
    enabled: !!props.cfg.sched_enabled,
    rows: list.map(e => ({
      enabled: e.enabled !== false,
      time: `${pad(e.hour)}:${pad(e.minute)}`,
      action: e.action === 1 ? 1 : 0,
      days: (e.days & 0x7F) || EVERY,
    })),
  };
}

const state = ref(fromCfg());
watch(() => props.cfg, () => {
  state.value = fromCfg();
  tz.value = props.cfg.timezone || 'UTC0';
});

const full = computed(() => state.value.rows.length >= SCHED_MAX);

function addRow() {
  if (full.value) return;
  state.value.rows.push({ enabled: true, time: '06:00', action: 1, days: EVERY });
}
function removeRow(i) { state.value.rows.splice(i, 1); }
function toggleDay(row, bit) {
  row.days ^= bit;
  if (!(row.days & 0x7F)) row.days = bit; /* never leave an entry with no day */
}

/* Human-readable summary of which days a row's bitmask covers. */
function daysLabel(mask) {
  if (mask === EVERY) return 'Every day';
  if (mask === WEEKDAYS) return 'Weekdays';
  if (mask === WEEKENDS) return 'Weekends';
  return DAYS.filter(d => mask & d.bit).map(d => d.title.slice(0, 3)).join(', ') || 'Never';
}

async function save() {
  const schedule = [];
  for (const r of state.value.rows) {
    const m = /^(\d{1,2}):(\d{2})$/.exec(r.time || '');
    if (!m) continue;
    const hour = Math.min(23, Math.max(0, +m[1]));
    const minute = Math.min(59, Math.max(0, +m[2]));
    schedule.push({
      hour, minute,
      action: r.action ? 1 : 0,
      days: (r.days & 0x7F) || EVERY,
      enabled: !!r.enabled,
    });
  }
  /* No timezone concept in AP mode — the manual clock runs in UTC0, so don't
   * push (and risk clobbering) the stored POSIX TZ. */
  const payload = { sched_enabled: !!state.value.enabled, schedule };
  if (!isAP.value) payload.timezone = tz.value;
  try {
    await props.onSave(payload);
    toast.add({ severity: 'success', summary: 'Schedule saved', life: 2000 });
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}
</script>

<template>
  <div class="stack">
    <!-- The clock the schedule actually fires on. Showing it makes a wrong
         timezone obvious at a glance. -->
    <div class="tile clock-tile">
      <div class="spaced">
        <span class="muted">Device time</span>
        <span class="clock-val" :class="{ stale: !clockReady }">
          {{ clockReady ? deviceTime : 'clock not set' }}
        </span>
      </div>

      <!-- STA keeps a timezone selector — used once the device is online and
           SNTP supplies real UTC. -->
      <div v-if="!isAP" class="row tz-row">
        <label class="muted" for="sched-tz">Timezone</label>
        <Select inputId="sched-tz" v-model="tz" :options="ZONES"
                optionLabel="label" optionValue="value" filter class="tz-sel" />
      </div>

      <!-- Manual clock: AP always, or any mode that can't (yet) sync. -->
      <div v-if="showManual" class="row tz-row set-time-row">
        <label class="muted" for="sched-time">Set time</label>
        <input id="sched-time" type="datetime-local" v-model="manualTime" class="dt-input" />
        <Button label="Set" icon="pi pi-check" size="small" @click="setManualTime" />
      </div>
    </div>

    <!-- No usable clock yet: scheduler is off until one is set. -->
    <div v-if="!clockReady" class="tile warn-banner">
      Scheduled actions won’t run until the device clock is set. Set it
      <strong>above</strong> to start the scheduler now.<template v-if="!isAP">
      The device also switches to automatic time on its own once it reaches the
      internet.</template> A manually set clock <strong>is lost on reboot</strong>,
      after which the scheduler stays off until you set it again.
    </div>

    <!-- Running on a hand-set clock — make the reboot behaviour explicit. -->
    <div v-else-if="clockManual" class="tile warn-banner">
      <strong>This clock was set manually and isn’t kept across a reboot.</strong>
      After a power cycle the scheduler stays off until you set the time again.<template v-if="!isAP">
      It will switch to automatic time once the device can sync over the
      internet.</template><template v-else> For automatic, persistent timekeeping,
      connect the device to Wi-Fi in <strong>station mode</strong>.</template>
    </div>

    <div class="sched-body" :class="{ locked: !clockReady }">
      <div class="spaced">
        <label class="row" style="gap: 8px; cursor: pointer;">
          <Checkbox v-model="state.enabled" :binary="true" inputId="sched-enabled" />
          <span>Enable scheduler</span>
        </label>
        <Button label="Add" icon="pi pi-plus" size="small" text
                :disabled="full" @click="addRow" />
      </div>

      <p class="muted" style="margin: -4px 0 0; font-size: 12px;">
        Actions fire at the set time (device-local) on the selected days.
      </p>

      <div v-if="!state.rows.length" class="muted tile" style="text-align: center;">
        No scheduled actions. Use “Add” to create one.
      </div>

    <div v-for="(row, i) in state.rows" :key="i" class="tile sched-row"
         :class="{ disabled: !row.enabled }" :title="daysLabel(row.days)">
      <Checkbox v-model="row.enabled" :binary="true" />
      <input type="time" v-model="row.time" class="time-input" />
      <Select v-model="row.action" :options="ACTIONS"
              optionLabel="label" optionValue="value" class="action-sel" />
      <div class="sched-days">
        <button v-for="(d, di) in DAYS" :key="di" type="button"
                class="day-chip" :class="{ on: row.days & d.bit }"
                :title="d.title" @click="toggleDay(row, d.bit)">{{ d.label }}</button>
      </div>
      <Button icon="pi pi-trash" severity="danger" text rounded class="sched-del"
              aria-label="Remove" @click="removeRow(i)" />
    </div>
    </div><!-- /.sched-body -->

    <Button label="Save schedule" @click="save" />
  </div>
</template>

<style scoped>
.clock-tile { display: flex; flex-direction: column; gap: 10px; }
.clock-val { font-variant-numeric: tabular-nums; font-weight: 600; }
.clock-val.stale { color: var(--muted); font-weight: 400; font-style: italic; }
.tz-row { gap: 10px; align-items: center; }
.tz-row label { min-width: 80px; }
/* min-width:0 lets the select shrink below its (long) label's intrinsic width
 * so it can't push the dialog wider than the viewport; the label ellipsizes. */
.tz-sel { flex: 1; min-width: 0; }
.tz-sel :deep(.p-select-label) { overflow: hidden; text-overflow: ellipsis; }

.set-time-row .dt-input {
  flex: 1;
  min-width: 0;
  font: inherit;
  color: var(--text);
  background: var(--surface-2);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 8px;
}
.set-time-row .dt-input::-webkit-calendar-picker-indicator { filter: invert(0.7); }

.warn-banner {
  border: 1px solid color-mix(in srgb, var(--accent) 40%, var(--border));
  background: color-mix(in srgb, var(--accent) 12%, transparent);
  font-size: 13px;
  line-height: 1.45;
}

/* When the clock can't sync (AP mode / no NTP) the schedule can't run, so the
 * editor is inert — dim it and swallow clicks. Timezone + Save stay live. */
.sched-body { display: flex; flex-direction: column; gap: 12px; }
.sched-body.locked { opacity: 0.5; pointer-events: none; filter: grayscale(0.3); }

.sched-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
}
.sched-row.disabled { opacity: 0.55; }

.action-sel { width: 100px; }

.time-input {
  font: inherit;
  color: var(--text);
  background: var(--surface-2);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 8px;
}
.time-input::-webkit-calendar-picker-indicator { filter: invert(0.7); }

/* Day chips take the slack between the action select and the trash button. */
.sched-days { display: flex; gap: 4px; flex: 1; justify-content: center; }
.day-chip {
  width: 26px; height: 26px;
  border-radius: 50%;
  border: 1px solid var(--border);
  background: var(--surface-2);
  color: var(--muted);
  font-size: 11px;
  cursor: pointer;
  transition: background 0.12s, color 0.12s, border-color 0.12s;
}
.day-chip.on {
  background: var(--accent);
  border-color: var(--accent);
  color: #fff;
}

/* On phones the single row overflows: time + action + 7 day-chips + trash is
 * wider than the screen. Wrap into a two-line card — controls on line 1, the
 * day chips spanning full width on line 2 (chips ordered after the trash so
 * they drop below it, not between the controls). */
@media (max-width: 520px) {
  .sched-row { flex-wrap: wrap; row-gap: 10px; }
  .time-input { flex: 1; min-width: 0; }
  .action-sel { width: auto; flex: 1; min-width: 0; }
  .sched-del { order: 1; margin-left: auto; }
  .sched-days {
    order: 2;
    flex: none;
    flex-basis: 100%;
    justify-content: space-between;
    gap: 4px;
  }
  .day-chip { width: 32px; height: 32px; font-size: 12px; }
}
</style>
