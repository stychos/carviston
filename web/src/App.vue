<script setup>
import { onMounted, computed } from 'vue';
import Toast from 'primevue/toast';
import ConfirmDialog from 'primevue/confirmdialog';
import { authStatus, refreshStatus } from './composables/auth.js';
import { connectLive } from './composables/liveState.js';

/* The three top-level views are mutually exclusive — only one is ever mounted
 * at a time, gated by `view`. They are imported EAGERLY (not as lazy
 * defineAsyncComponent chunks) on purpose: PrimeVue's Aura theme tokens are
 * injected by runtime JS as each component mounts, so a view that arrives in a
 * deferred chunk can paint before its theme is applied. That shows up most
 * sharply during first-boot in Wi-Fi AP mode — a phone/laptop on the open
 * SoftAP has no internet, the OS opens the page in a constrained/captive
 * context, and the on-demand chunk + its runtime theme land late, so the
 * widgets render unstyled until STA is configured. Folding all three views into
 * the entry bundle (≈75 KB total, embedded in flash) removes that round-trip
 * entirely; first paint is fully themed. Code-splitting a single-appliance UI
 * that always lands on the dashboard bought nothing on a LAN anyway. */
import Setup     from './views/Setup.vue';
import Login     from './views/Login.vue';
import Dashboard from './views/Dashboard.vue';
import ThemeSwitch from './components/ThemeSwitch.vue';

/* Build-time constant injected by vite.config.js (define). Shape: 0.<config
 * schema>.<build counter>. Baked in, so it shows immediately with no API call. */
/* global __APP_VERSION__ */
const version = __APP_VERSION__;

onMounted(async () => {
  /* Keep asking until the device answers. On a flaky link the first
   * /api/auth/status may time out; we must NOT fall back to a guessed view
   * (that's what caused the Setup-form flash), so we hold the spinner and
   * retry until we actually know what to render. */
  while (!authStatus.value) {
    await refreshStatus();
    if (!authStatus.value) await new Promise((r) => setTimeout(r, 1500));
  }
  /* Open dashboard always tries to live-stream; the WS backend will accept
   * an empty token when the dashboard isn't locked. */
  if (authStatus.value.authenticated || !authStatus.value.dashboard_locked) {
    connectLive();
  }
});

const view = computed(() => {
  /* Status not known yet — show the boot spinner, never a guessed screen. */
  if (!authStatus.value) return 'loading';
  if (!authStatus.value.configured) return 'setup';
  /* Login screen only when the dashboard is explicitly locked AND the
   * user hasn't signed in yet. Otherwise everyone lands on the dashboard;
   * the settings button does its own auth check before opening. */
  if (authStatus.value.dashboard_locked && !authStatus.value.authenticated) return 'login';
  return 'dashboard';
});
</script>

<template>
  <!-- Theme (.dark-mode) is applied on <html> by composables/theme.js, which
       drives both our CSS-variable palette and PrimeVue's Aura dark selector. -->
  <div class="app-root">
    <!-- Boot spinner: same classes as the static splash in index.html, so the
         cached-load handoff (static markup → Vue render) is seamless. -->
    <div v-if="view === 'loading'" class="boot-splash">
      <div class="boot-spinner"></div>
      <div class="boot-status">Loading&hellip;</div>
    </div>
    <Setup     v-else-if="view === 'setup'" />
    <Login     v-else-if="view === 'login'" />
    <Dashboard v-else />

    <footer class="app-footer">
      <div class="app-footer-inner">
        <span class="muted">v{{ version }}</span>
        <ThemeSwitch />
      </div>
    </footer>

    <Toast position="bottom-center" />
    <ConfirmDialog />
  </div>
</template>
