<script setup>
import { reactive, watch } from 'vue';
import InputNumber from 'primevue/inputnumber';
import Button from 'primevue/button';
import { useToast } from 'primevue/usetoast';
import { tempField, unitSuffix } from '../../composables/units.js';

const props = defineProps({
  cfg: Object,
  onSave: { type: Function, required: true },
});
const toast = useToast();

const form = reactive({ ...props.cfg });
watch(() => props.cfg, c => Object.assign(form, c));

/* Temperature fields edit in the active unit but persist canonical Celsius.
 * shower-ready is an absolute temp; hysteresis and mismatch are spans (delta). */
const { model: showerReady, min: showerMin, max: showerMax } =
  tempField(form, 'shower_ready_c', 30, 70);
const { model: hysteresis, min: hystMin, max: hystMax } =
  tempField(form, 'hysteresis_c', 1, 10, { delta: true });
/* Per-tank probe-disagreement windows. The inlet stratifies/cools faster on a
 * draw, so its two NTCs read further apart — it gets a wider window. */
const { model: mismatchInlet, min: mismatchInMin, max: mismatchInMax } =
  tempField(form, 'probe_disagree_inlet_c', 1, 30, { delta: true });
const { model: mismatchOutlet, min: mismatchOutMin, max: mismatchOutMax } =
  tempField(form, 'probe_disagree_outlet_c', 1, 20, { delta: true });

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
        <label class="muted">Hysteresis ({{ unitSuffix }})</label>
        <InputNumber v-model="hysteresis" :min="hystMin" :max="hystMax" fluid showButtons />
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
      <label class="muted">Shower-ready threshold ({{ unitSuffix }})</label>
      <InputNumber v-model="showerReady" :min="showerMin" :max="showerMax" fluid showButtons />
    </div>

    <details class="muted" style="margin-top: 4px;">
      <summary style="cursor: pointer;">Advanced — temperature sensor calibration</summary>
      <div class="row" style="gap: 12px; margin-top: 10px;">
        <div style="flex:1">
          <label class="muted">R25 (ohm)</label>
          <InputNumber v-model="form.ntc_r25_ohm" :min="1000" :max="1000000" :step="100" fluid />
        </div>
        <div style="flex:1">
          <label class="muted">Beta</label>
          <InputNumber v-model="form.ntc_beta" :min="2000" :max="5500" fluid />
        </div>
      </div>
      <div class="row" style="gap: 12px; margin-top: 10px;">
        <div style="flex:1">
          <label class="muted">Inlet mismatch ({{ unitSuffix }})</label>
          <InputNumber v-model="mismatchInlet" :min="mismatchInMin" :max="mismatchInMax" fluid />
        </div>
        <div style="flex:1">
          <label class="muted">Outlet mismatch ({{ unitSuffix }})</label>
          <InputNumber v-model="mismatchOutlet" :min="mismatchOutMin" :max="mismatchOutMax" fluid />
        </div>
      </div>
    </details>

    <Button label="Save heater settings" @click="save" />
  </div>
</template>
