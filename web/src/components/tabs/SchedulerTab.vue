<script setup>
import { ref, watch, computed } from 'vue';
import Button from 'primevue/button';
import Checkbox from 'primevue/checkbox';
import Select from 'primevue/select';
import { useToast } from 'primevue/usetoast';
import { state as liveState } from '../../composables/liveState.js';

const props = defineProps({
  cfg: Object,
  onSave: { type: Function, required: true },
});
const toast = useToast();

const SCHED_MAX = 16;

/* The device reasons about schedule times in LOCAL wall-clock, which only works
 * once SNTP has synced — impossible in AP mode (no internet). The firmware
 * refuses to fire an unsynced schedule; here we mirror that by locking the
 * editor and showing the device's own clock so a wrong timezone is obvious. */
const deviceTime = computed(() => liveState.value?.device_time || null);
const clockReady = computed(() => liveState.value?.time_synced === true);

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

const pad = n => String(n).padStart(2, '0');

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
  try {
    await props.onSave({ sched_enabled: !!state.value.enabled, schedule, timezone: tz.value });
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
          {{ clockReady ? deviceTime : 'clock not synced' }}
        </span>
      </div>
      <div class="row tz-row">
        <label class="muted" for="sched-tz">Timezone</label>
        <Select inputId="sched-tz" v-model="tz" :options="ZONES"
                optionLabel="label" optionValue="value" filter class="tz-sel" />
      </div>
    </div>

    <div v-if="!clockReady" class="tile warn-banner">
      The clock hasn’t synced, so scheduled actions won’t run. Time sync needs
      Wi-Fi <strong>station mode</strong> with internet — it can’t happen in AP
      mode. Connect the device to your network and the scheduler activates
      automatically once the clock syncs.
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
      <Button icon="pi pi-trash" severity="danger" text rounded
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
.tz-sel { flex: 1; }

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

@media (max-width: 520px) {
  .sched-days { gap: 3px; }
  .day-chip { width: 24px; height: 24px; }
}
</style>
