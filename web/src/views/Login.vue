<script setup>
import { ref } from 'vue';
import Password from 'primevue/password';
import Button from 'primevue/button';
import Message from 'primevue/message';
import { login } from '../composables/auth.js';
import { connectLive } from '../composables/liveState.js';

const pw  = ref('');
const err = ref('');
const busy = ref(false);

async function submit() {
  err.value = '';
  busy.value = true;
  try {
    await login(pw.value);
    connectLive();
  } catch {
    err.value = 'Wrong password.';
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="center">
    <div class="card">
      <h2 style="margin:0 0 4px">Carviston</h2>
      <div class="muted" style="margin-bottom:20px">Sign in.</div>
      <div class="stack">
        <Password v-model="pw" toggleMask :feedback="false" fluid
                  :inputProps="{ autofocus: true }"
                  @keydown.enter="submit" />
        <Message v-if="err" severity="error" :closable="false">{{ err }}</Message>
        <Button label="Sign in" :loading="busy" @click="submit" />
      </div>
    </div>
  </div>
</template>
