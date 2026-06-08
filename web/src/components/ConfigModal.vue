<script setup>
import { ref, watch } from 'vue';
import Dialog from 'primevue/dialog';
import Tabs from 'primevue/tabs';
import TabList from 'primevue/tablist';
import Tab from 'primevue/tab';
import TabPanels from 'primevue/tabpanels';
import TabPanel from 'primevue/tabpanel';
import { api } from '../composables/api.js';

/* Tabs are imported EAGERLY. The whole UI is embedded in the firmware and
 * served from flash over the LAN, so code-splitting buys no network win — it
 * only adds an on-demand chunk fetch that can land late or unstyled in a
 * constrained context (e.g. a captive first-boot AP). Folding the tabs into
 * the entry bundle keeps the dialog instant and removes that failure mode. */
import BoilerTab      from './tabs/BoilerTab.vue';
import AppTab         from './tabs/AppTab.vue';
import WifiTab        from './tabs/WifiTab.vue';
import MaintenanceTab from './tabs/MaintenanceTab.vue';
import LogsTab        from './tabs/LogsTab.vue';
import SchedulerTab   from './tabs/SchedulerTab.vue';

const props = defineProps({ visible: Boolean });
const emit  = defineEmits(['update:visible']);
const visible = ref(props.visible);
watch(() => props.visible, v => visible.value = v);
watch(visible, v => emit('update:visible', v));

const cfg = ref(null);
const activeTab = ref('heater');

async function load() {
  try { cfg.value = await api.get('/api/config'); }
  catch { cfg.value = null; }
}
watch(visible, (v) => { if (v) load(); });

async function save(patch) {
  cfg.value = await api.put('/api/config', patch);
}
</script>

<template>
  <Dialog v-model:visible="visible" modal class="config-dialog"
          :style="{ width: 'min(720px, 96vw)' }"
          :draggable="false" :dismissableMask="true" header="Configuration">
    <template v-if="cfg">
      <Tabs v-model:value="activeTab">
        <TabList>
          <Tab value="heater">Heater</Tab>
          <Tab value="app">App</Tab>
          <Tab value="wifi">Wi-Fi</Tab>
          <Tab value="logs">Logs</Tab>
          <Tab value="scheduler">Scheduler</Tab>
          <Tab value="maintenance">Maintenance</Tab>
        </TabList>
        <TabPanels>
          <!-- Pass `save` as an async callback prop, not as an event listener:
               Vue's emit() returns void/undefined synchronously, so a child
               doing `await emit('save', …)` cannot observe a rejection from
               the parent's async handler. A function prop preserves the
               Promise so try/catch in the child actually catches failures. -->
          <TabPanel value="heater"><BoilerTab :cfg="cfg" :on-save="save" /></TabPanel>
          <TabPanel value="app"><AppTab :cfg="cfg" :on-save="save" /></TabPanel>
          <TabPanel value="wifi"><WifiTab :cfg="cfg" :on-save="save" /></TabPanel>
          <TabPanel value="logs"><LogsTab /></TabPanel>
          <TabPanel value="scheduler"><SchedulerTab :cfg="cfg" :on-save="save" /></TabPanel>
          <TabPanel value="maintenance"><MaintenanceTab /></TabPanel>
        </TabPanels>
      </Tabs>
    </template>
    <div v-else class="muted" style="padding: 16px;">Loading…</div>
  </Dialog>
</template>
