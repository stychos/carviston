<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue';
import DataTable from 'primevue/datatable';
import Column from 'primevue/column';
import Button from 'primevue/button';
import { useConfirm } from 'primevue/useconfirm';
import { useToast } from 'primevue/usetoast';
import { api } from '../../composables/api.js';
import { isFahrenheit, cToF, unitSuffix } from '../../composables/units.js';

const confirm = useConfirm();
const toast = useToast();
const data = ref({ events: [], current_boot: 0 });
const loading = ref(false);
let timer = null;

async function refresh() {
  loading.value = true;
  try { data.value = await api.get('/api/log'); }
  catch {}
  finally { loading.value = false; }
}

/* Both clears are destructive and irreversible, so gate behind a confirm.
 * Clearing the log also empties the heating-performance table — those rows are
 * derived from bench_* events in the same log — which the message makes plain. */
function clearLog() {
  confirm.require({
    header: 'Clear event log',
    message: 'Delete all event-log entries? This also clears the heating performance history.',
    icon: 'pi pi-trash',
    acceptLabel: 'Clear', rejectLabel: 'Cancel', acceptProps: { severity: 'danger' },
    accept: async () => {
      try { await api.post('/api/log/clear'); await refresh();
            toast.add({ severity: 'success', summary: 'Event log cleared', life: 1500 }); }
      catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
    },
  });
}
function clearBenchmarks() {
  confirm.require({
    header: 'Clear heating performance',
    message: 'Delete all recorded heating runs? The rest of the event log is kept.',
    icon: 'pi pi-trash',
    acceptLabel: 'Clear', rejectLabel: 'Cancel', acceptProps: { severity: 'danger' },
    accept: async () => {
      try { await api.post('/api/benchmarks/clear'); await refresh();
            toast.add({ severity: 'success', summary: 'Heating performance cleared', life: 1500 }); }
      catch (e) { toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 }); }
    },
  });
}

onMounted(() => { refresh(); timer = setInterval(refresh, 5000); });
onUnmounted(() => { if (timer) clearInterval(timer); });

/* Map snake_case event type → human readable label. Unknown types fall
 * through to the original string so log entries stay debuggable when the
 * backend adds something new. */
const EVENT_LABELS = {
  boot: 'Started up', shutdown: 'Shutting down', reboot_request: 'Reboot requested',
  factory_reset: 'Hardware reset',
  wifi_ap: 'Hotspot started',
  wifi_sta_connected: 'Connected to Wi-Fi', wifi_sta_disconnected: 'Wi-Fi connection lost',
  time_synced: 'Clock synchronised',
  button_press: 'Button pressed', button_long: 'Button held',
  mode_change: 'Mode changed', target_change: 'Target changed',
  master_on: 'Heaters turned on', master_off: 'Heaters turned off',
  heater_on: 'Heater on', heater_off: 'Heater off',
  safety_fault: 'Safety issue', safety_cleared: 'Safety cleared',
  ota_start: 'Update started', ota_done: 'Update complete', ota_fail: 'Update failed',
  bench_start: 'Heating test started', bench_end: 'Heating test complete', bench_abort: 'Heating test cancelled',
};
function labelFor(type) { return EVENT_LABELS[type] || type; }

const MODE_NAMES = ['Super-fast', 'Fast', 'Optimal', 'Eco'];
const BUTTON_NAMES = ['Power', 'ECO', 'Shower', 'Plus', 'Minus'];
const SAFETY_NAMES = ['All clear', 'Safety switch tripped', 'Sensor problem', 'Water too hot', 'No sensors'];

function fmtAbsolute(ev) {
  if (ev.ts > 1700000000) {
    return new Date(ev.ts * 1000).toLocaleString(undefined, { hour12: false });
  }
  return `boot ${ev.boot} +${ev.uptime_s}s`;
}

const _rel_units = [
  { ms: 60000, n: 'm' },
  { ms: 3600000, n: 'h' },
  { ms: 86400000, n: 'd' },
];
function fmtRelative(ev) {
  if (!(ev.ts > 1700000000)) return fmtAbsolute(ev);
  const delta = Date.now() - ev.ts * 1000;
  if (delta < 60000) return 'just now';
  for (let i = _rel_units.length - 1; i >= 0; --i) {
    if (delta >= _rel_units[i].ms) {
      return `${Math.floor(delta / _rel_units[i].ms)}${_rel_units[i].n} ago`;
    }
  }
  return 'just now';
}

/* Benchmark start/end temps are stored Celsius; show them in the active unit. */
function dispTemp(c) {
  return (isFahrenheit.value ? cToF(c) : c).toFixed(1);
}

function fmtPayload(ev) {
  if (ev.text) return ev.text;
  switch (ev.type) {
    case 'target_change': return `→ ${isFahrenheit.value ? Math.round(cToF(ev.a)) : ev.a}${unitSuffix.value}`;
    case 'mode_change':   return MODE_NAMES[ev.a] || `mode ${ev.a}`;
    case 'button_press':
    case 'button_long':   return BUTTON_NAMES[ev.a] || `button ${ev.a}`;
    case 'heater_on':
    case 'heater_off':    return `heater ${ev.a + 1}`;
    case 'safety_fault':  return SAFETY_NAMES[ev.a] || `#${ev.a}`;
    case 'reboot_request':
      return ev.a === 0 || ev.a === 1 ? `→ Firmware ${ev.a === 0 ? 'A' : 'B'}` : '';
    default: return '';
  }
}

function severityFor(type) {
  if (type === 'safety_fault' || type === 'ota_fail')             return 'danger';
  if (type === 'wifi_sta_disconnected' || type === 'bench_abort') return 'warn';
  if (type === 'ota_done' || type === 'bench_end'
      || type === 'wifi_sta_connected' || type === 'safety_cleared'
      || type === 'master_on')                                    return 'success';
  if (type === 'boot' || type === 'shutdown'
      || type === 'reboot_request' || type === 'time_synced')     return 'info';
  return 'secondary';
}

/* Trigger a re-render every minute so the "Xm ago" stays fresh without a
 * full refresh. */
const _now = ref(Date.now());
let _now_tick = null;
onMounted(() => { _now_tick = setInterval(() => { _now.value = Date.now(); }, 60000); });
onUnmounted(() => { if (_now_tick) clearInterval(_now_tick); });
const eventsView = computed(() => {
  void _now.value;   /* reactivity nudge */
  return data.value.events;
});

const benchmarks = computed(() => {
  const starts = new Map();
  const out = [];
  for (let i = data.value.events.length - 1; i >= 0; --i) {
    const e = data.value.events[i];
    if (e.type === 'bench_start') starts.set(e.boot + ':' + e.uptime_s, e);
    else if (e.type === 'bench_end' || e.type === 'bench_abort') {
      const startKeys = [...starts.entries()].filter(([k]) => k.startsWith(e.boot + ':'));
      const start = startKeys.length ? startKeys[startKeys.length - 1][1] : null;
      out.push({
        start_ts: start ? start.ts : 0,
        end_ts:   e.ts,
        start_c:  start ? start.a / 10 : (e.a / 10),
        end_c:    e.b / 10,
        mode:     start ? start.b : null,
        text:     e.text,
        aborted:  e.type === 'bench_abort',
      });
    }
  }
  return out.slice(-30).reverse();
});
</script>

<template>
  <div class="stack">
    <div class="spaced">
      <h4 style="margin: 0;">Event log</h4>
      <div class="row" style="gap: 2px;">
        <Button label="Clear" size="small" icon="pi pi-trash" text severity="danger"
                :disabled="!eventsView.length" @click="clearLog" />
        <Button label="Refresh" size="small" icon="pi pi-refresh" text :loading="loading" @click="refresh" />
      </div>
    </div>

    <div v-if="!eventsView.length" class="muted" style="padding: 24px 0; text-align: center;">
      No events yet.
    </div>
    <div v-else class="log-list">
      <div v-for="(ev, i) in eventsView" :key="i" class="log-row">
        <div class="log-time" :title="fmtAbsolute(ev)">{{ fmtRelative(ev) }}</div>
        <span class="log-type" :class="`log-type-${severityFor(ev.type)}`">{{ labelFor(ev.type) }}</span>
        <div class="log-detail">{{ fmtPayload(ev) }}</div>
      </div>
    </div>

    <div class="spaced" style="margin-top: 16px;">
      <h4 style="margin: 0;">Heating performance</h4>
      <Button label="Clear" size="small" icon="pi pi-trash" text severity="danger"
              :disabled="!benchmarks.length" @click="clearBenchmarks" />
    </div>
    <DataTable v-if="benchmarks.length" :value="benchmarks" size="small" stripedRows
               scrollable scrollHeight="200px" :paginator="false">
      <Column header="Started">
        <template #body="{ data }">
          <span v-if="data.start_ts" class="muted" style="font-size: 12px;">
            {{ new Date(data.start_ts * 1000).toLocaleString(undefined, { hour12: false }) }}
          </span>
          <span v-else class="muted">—</span>
        </template>
      </Column>
      <Column header="From → to">
        <template #body="{ data }">
          <span v-if="data.start_c != null">{{ dispTemp(data.start_c) }}° → {{ dispTemp(data.end_c) }}°</span>
          <span v-else class="muted">—</span>
        </template>
      </Column>
      <Column header="Mode">
        <template #body="{ data }">
          <span v-if="data.mode != null" style="font-size: 12px;">{{ MODE_NAMES[data.mode] || data.mode }}</span>
          <span v-else class="muted">—</span>
        </template>
      </Column>
      <Column header="Result">
        <template #body="{ data }">
          <span :class="data.aborted ? 'muted' : ''" style="font-family: ui-monospace, monospace; font-size: 12px;">
            {{ data.text }}
          </span>
        </template>
      </Column>
    </DataTable>
    <div v-else class="muted" style="padding: 16px 0; text-align: center;">
      No heating runs recorded yet.
    </div>
  </div>
</template>

<style scoped>
.log-list {
  max-height: 360px; overflow: auto;
  border: 1px solid var(--border); border-radius: 10px;
  background: var(--surface-2);
}
.log-row {
  display: grid;
  grid-template-columns: 90px auto 1fr;
  gap: 12px; align-items: center;
  padding: 8px 12px;
  border-bottom: 1px solid var(--border);
  font-size: 13px;
}
.log-row:last-child { border-bottom: none; }
.log-row:hover { background: rgba(255,255,255,0.02); }

.log-time {
  font-size: 11px; color: var(--muted);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}
.log-type {
  font-size: 13px;
  font-weight: 600;
  white-space: nowrap;
  color: var(--text);
}
.log-type-success   { color: var(--good); }
.log-type-warn      { color: var(--warn); }
.log-type-danger    { color: var(--bad);  }
.log-type-info      { color: var(--accent); }
.log-type-secondary { color: var(--text); }
.log-detail {
  color: var(--text);
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
</style>
