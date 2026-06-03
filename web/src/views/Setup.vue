<script setup>
import { ref, computed } from 'vue';
import Password from 'primevue/password';
import Button from 'primevue/button';
import Message from 'primevue/message';
import { setupPassword } from '../composables/auth.js';
import { connectLive } from '../composables/liveState.js';

const pw  = ref('');
const pw2 = ref('');
const err = ref('');
const busy = ref(false);

const canGo = computed(() => pw.value.length >= 4 && pw.value === pw2.value);

async function submit() {
  err.value = '';
  if (!canGo.value) return;
  busy.value = true;
  try {
    await setupPassword(pw.value);
    connectLive();
  } catch (e) {
    err.value = e.message || 'failed';
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="center">
    <div class="card">
      <h2 style="margin:0 0 4px">Welcome to Carviston</h2>
      <div class="muted" style="margin-bottom:20px">
        Set an admin password to finish first-time setup.
      </div>
      <div class="stack">
        <div>
          <label class="muted">Password</label>
          <Password v-model="pw" toggleMask :feedback="false" inputClass="w-full"
                    :inputProps="{ autofocus: true }" fluid />
        </div>
        <div>
          <label class="muted">Confirm</label>
          <Password v-model="pw2" toggleMask :feedback="false" fluid />
        </div>
        <Message v-if="err" severity="error" :closable="false">{{ err }}</Message>
        <Button label="Save and continue" :disabled="!canGo" :loading="busy" @click="submit" />
        <div class="muted" style="font-size: 12px; text-align: center;">
          Used to protect the settings page. Stored securely on the device.
        </div>
      </div>
    </div>
  </div>
</template>
