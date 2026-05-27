<script setup>
import { reactive, watch } from 'vue';
import Select from 'primevue/select';
import InputNumber from 'primevue/inputnumber';
import Button from 'primevue/button';
import { useToast } from 'primevue/usetoast';

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

const form = reactive({ ...props.cfg });
watch(() => props.cfg, c => Object.assign(form, c));

async function save() {
  /* heating_mode lives on the dashboard now — don't echo it back from the
   * stale form copy or we'd clobber a mode change the user made while the
   * modal was open. */
  const { heating_mode: _drop, ...patch } = form;
  try {
    await props.onSave(patch);
    toast.add({ severity: 'success', summary: 'Saved', life: 2000 });
  } catch (e) {
    toast.add({ severity: 'error', summary: 'Failed', detail: e.message, life: 3000 });
  }
}
</script>

<template>
  <div class="stack">
    <div class="row" style="gap: 12px;">
      <div style="flex:1">
        <label class="muted">Fast: on (min)</label>
        <InputNumber v-model="form.fast_on_min" :min="1" :max="120" fluid showButtons />
      </div>
      <div style="flex:1">
        <label class="muted">Fast: rest (min)</label>
        <InputNumber v-model="form.fast_rest_min" :min="0" :max="120" fluid showButtons />
      </div>
    </div>
    <div class="row" style="gap: 12px;">
      <div style="flex:1">
        <label class="muted">Optimal: swap (min)</label>
        <InputNumber v-model="form.optimal_swap_min" :min="1" :max="120" fluid showButtons />
      </div>
      <div style="flex:1">
        <label class="muted">Hysteresis (°C)</label>
        <InputNumber v-model="form.hysteresis_c" :min="1" :max="10" fluid showButtons />
      </div>
    </div>
    <div class="row" style="gap: 12px;">
      <div style="flex:1">
        <label class="muted">Eco: on (min)</label>
        <InputNumber v-model="form.eco_on_min" :min="1" :max="120" fluid showButtons />
      </div>
      <div style="flex:1">
        <label class="muted">Eco: rest (min)</label>
        <InputNumber v-model="form.eco_rest_min" :min="0" :max="240" fluid showButtons />
      </div>
    </div>

    <div>
      <label class="muted">Shower-ready threshold (°C)</label>
      <InputNumber v-model="form.shower_ready_c" :min="30" :max="70" fluid showButtons />
    </div>

    <div class="row" style="gap: 12px;">
      <div style="flex:1">
        <label class="muted">Power LED</label>
        <Select v-model="form.power_led_mode" :options="POWER_LED_MODES" optionLabel="l" optionValue="v" fluid />
      </div>
      <div style="flex:1">
        <label class="muted">ECO LED</label>
        <Select v-model="form.eco_led_mode" :options="ECO_LED_MODES" optionLabel="l" optionValue="v" fluid />
      </div>
    </div>
    <div>
      <label class="muted">Dashboard unit</label>
      <Select v-model="form.dashboard_unit" :options="UNITS" optionLabel="l" optionValue="v" fluid />
    </div>

    <details class="muted" style="margin-top: 4px;">
      <summary style="cursor: pointer;">NTC calibration</summary>
      <div class="row" style="gap: 12px; margin-top: 10px;">
        <div style="flex:1">
          <label class="muted">R25 (ohm)</label>
          <InputNumber v-model="form.ntc_r25_ohm" :min="1000" :max="1000000" :step="100" fluid />
        </div>
        <div style="flex:1">
          <label class="muted">Beta</label>
          <InputNumber v-model="form.ntc_beta" :min="2000" :max="5500" fluid />
        </div>
        <div style="flex:1">
          <label class="muted">Probe disagree (°C)</label>
          <InputNumber v-model="form.probe_disagree_c" :min="1" :max="20" fluid />
        </div>
      </div>
    </details>

    <Button label="Save boiler settings" @click="save" />
  </div>
</template>
