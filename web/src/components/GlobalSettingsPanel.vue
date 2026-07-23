<template>
  <Teleport to="body">
    <div v-if="open" class="settings-backdrop" @pointerdown.self="close">
      <section
        ref="panelElement"
        class="baklava-node settings-panel"
        role="dialog"
        aria-modal="true"
        aria-labelledby="global-settings-title"
        @pointerdown.stop
      >
        <header class="settings-header">
          <h1 id="global-settings-title">Global Settings</h1>
          <button type="button" class="settings-close" aria-label="Close settings" @click="close">
            ×
          </button>
        </header>

        <div v-for="section in sections" :key="section.title" class="settings-section">
          <h2>{{ section.title }}</h2>
          <component
            :is="field.intf.component"
            v-for="field in section.fields"
            :key="field.key"
            :model-value="field.intf.value"
            :node="node"
            :intf="field.intf"
            @update:model-value="field.intf.value = $event"
          />
        </div>
      </section>
    </div>
  </Teleport>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from "vue";
import type {
  ApplicationSettingsInterface,
  ApplicationSettingsView,
} from "../application_settings";

const props = defineProps<{
  open: boolean;
  node: ApplicationSettingsView;
}>();

const emit = defineEmits<{
  (event: "close"): void;
}>();

const panelElement = ref<HTMLElement>();

const sectionDefinitions = [
  { title: "Program", keys: ["frame_rate"] },
  {
    title: "DeckLink Output",
    keys: ["decklink_output_preroll_frames", "decklink_output_buffer_frames"],
  },
  { title: "NDI Output", keys: ["ndi_output_buffer_frames"] },
] as const;

interface SettingsField {
  key: string;
  intf: ApplicationSettingsInterface;
}

const sections = computed(() =>
  sectionDefinitions.map((section) => ({
    title: section.title,
    fields: section.keys.flatMap((key): SettingsField[] => {
      const inputs = props.node.inputs as Record<string, ApplicationSettingsInterface>;
      const intf = inputs[key];
      return intf?.component ? [{ key, intf }] : [];
    }),
  })),
);

function close(): void {
  emit("close");
}

function handleKeydown(event: KeyboardEvent): void {
  if (props.open && event.key === "Escape") {
    event.preventDefault();
    event.stopPropagation();
    close();
  }
}

watch(
  () => props.open,
  async (open) => {
    if (open) {
      window.addEventListener("keydown", handleKeydown);
      await nextTick();
      panelElement.value?.querySelector<HTMLElement>("[data-node-option-tab]")?.focus();
    } else {
      window.removeEventListener("keydown", handleKeydown);
    }
  },
  { immediate: true },
);

onBeforeUnmount(() => window.removeEventListener("keydown", handleKeydown));
</script>

<style scoped>
.settings-backdrop {
  position: fixed;
  inset: 0;
  z-index: 1000;
  display: grid;
  place-items: center;
  background: rgba(0, 0, 0, 0.55);
}

.settings-panel {
  position: static;
  width: min(28rem, calc(100vw - 2rem));
  max-height: calc(100vh - 2rem);
  overflow-y: auto;
  padding: 0;
  color: #e0e0e0;
  background: #242435;
  border: 1px solid rgba(120, 120, 160, 0.55);
  border-radius: 6px;
  box-shadow: 0 12px 36px rgba(0, 0, 0, 0.5);
}

.settings-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.75rem 1rem;
  border-bottom: 1px solid rgba(120, 120, 160, 0.35);
}

.settings-header h1 {
  margin: 0;
  font-size: 1.05rem;
  font-weight: 600;
}

.settings-close {
  width: 2rem;
  height: 2rem;
  padding: 0;
  color: inherit;
  font-size: 1.5rem;
  line-height: 1;
  cursor: pointer;
  background: transparent;
  border: 0;
  border-radius: 3px;
}

.settings-close:hover,
.settings-close:focus-visible {
  background: rgba(255, 255, 255, 0.1);
  outline: none;
}

.settings-section {
  padding: 0.8rem 1rem 1rem;
}

.settings-section + .settings-section {
  border-top: 1px solid rgba(120, 120, 160, 0.25);
}

.settings-section h2 {
  margin: 0 0 0.6rem;
  color: #b8bdd0;
  font-size: 0.78rem;
  font-weight: 600;
  letter-spacing: 0.06em;
  text-transform: uppercase;
}
</style>
