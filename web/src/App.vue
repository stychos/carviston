<script setup>
import { onMounted, computed } from 'vue';
import Toast from 'primevue/toast';
import ConfirmDialog from 'primevue/confirmdialog';
import Setup from './views/Setup.vue';
import Login from './views/Login.vue';
import Dashboard from './views/Dashboard.vue';
import { authStatus, refreshStatus } from './composables/auth.js';
import { connectLive } from './composables/liveState.js';

onMounted(async () => {
  await refreshStatus();
  /* Open dashboard always tries to live-stream; the WS backend will accept
   * an empty token when the dashboard isn't locked. */
  if (authStatus.value.authenticated || !authStatus.value.dashboard_locked) {
    connectLive();
  }
});

const view = computed(() => {
  if (!authStatus.value.configured) return 'setup';
  /* Login screen only when the dashboard is explicitly locked AND the
   * user hasn't signed in yet. Otherwise everyone lands on the dashboard;
   * the settings button does its own auth check before opening. */
  if (authStatus.value.dashboard_locked && !authStatus.value.authenticated) return 'login';
  return 'dashboard';
});
</script>

<template>
  <div class="dark-mode">
    <Setup     v-if="view === 'setup'" />
    <Login     v-else-if="view === 'login'" />
    <Dashboard v-else />
    <Toast position="bottom-center" />
    <ConfirmDialog />
  </div>
</template>
