<script setup>
import { ref, watch } from 'vue';
import Dialog from 'primevue/dialog';
import Tabs from 'primevue/tabs';
import TabList from 'primevue/tablist';
import Tab from 'primevue/tab';
import TabPanels from 'primevue/tabpanels';
import TabPanel from 'primevue/tabpanel';
import BoilerTab from './tabs/BoilerTab.vue';
import AppTab from './tabs/AppTab.vue';
import MaintenanceTab from './tabs/MaintenanceTab.vue';
import LogsTab from './tabs/LogsTab.vue';
import { api } from '../composables/api.js';

const props = defineProps({ visible: Boolean });
const emit  = defineEmits(['update:visible']);
const visible = ref(props.visible);
watch(() => props.visible, v => visible.value = v);
watch(visible, v => emit('update:visible', v));

const cfg = ref(null);
const activeTab = ref('0');

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
          <Tab value="0">Boiler</Tab>
          <Tab value="1">Wi-Fi</Tab>
          <Tab value="2">Logs</Tab>
          <Tab value="3">Maintenance</Tab>
        </TabList>
        <TabPanels>
          <!-- Pass `save` as an async callback prop, not as an event listener:
               Vue's emit() returns void/undefined synchronously, so a child
               doing `await emit('save', …)` cannot observe a rejection from
               the parent's async handler. A function prop preserves the
               Promise so try/catch in the child actually catches failures. -->
          <TabPanel value="0"><BoilerTab :cfg="cfg" :on-save="save" /></TabPanel>
          <TabPanel value="1"><AppTab    :cfg="cfg" :on-save="save" /></TabPanel>
          <TabPanel value="2"><LogsTab /></TabPanel>
          <TabPanel value="3"><MaintenanceTab /></TabPanel>
        </TabPanels>
      </Tabs>
    </template>
    <div v-else class="muted" style="padding: 16px;">Loading…</div>
  </Dialog>
</template>
