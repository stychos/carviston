<script setup>
import { theme, setTheme } from '../composables/theme.js';

const OPTIONS = [
  { v: 'auto',  label: 'Auto',  icon: 'pi pi-desktop' },
  { v: 'light', label: 'Light', icon: 'pi pi-sun' },
  { v: 'dark',  label: 'Dark',  icon: 'pi pi-moon' },
];
</script>

<template>
  <div class="theme-switch" role="group" aria-label="Colour theme">
    <button v-for="o in OPTIONS" :key="o.v" type="button"
            :class="{ active: theme === o.v }"
            :aria-pressed="theme === o.v"
            :title="`${o.label} theme`"
            @click="setTheme(o.v)">
      <i :class="o.icon" aria-hidden="true"></i>
      <span class="theme-switch-label">{{ o.label }}</span>
    </button>
  </div>
</template>
