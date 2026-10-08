<template>
  <div>
    <FocusTrackingStringOption
      :model-value="modelValue"
      :node="node"
      :intf="intf"
      @update:model-value="emit('update:modelValue', $event)"
    />
    <button v-if="available" type="button" :disabled="pending" @click="browse">Browse…</button>
    <div v-if="error" class="file-error" role="alert">{{ error }}</div>
  </div>
</template>

<script setup lang="ts">
import { inject, onBeforeUnmount, ref } from "vue";
import type { AbstractNode, NodeInterface } from "@baklavajs/core";
import { websocket_key } from "@/websocket";
import { requestFileDialog } from "@/file_dialog";
import { action_e } from "@/messages";
import FocusTrackingStringOption from "./FocusTrackingStringOption.vue";

const props = defineProps<{
  modelValue: string;
  node: AbstractNode;
  intf: NodeInterface<string>;
}>();
const emit = defineEmits<{ (e: "update:modelValue", value: string): void }>();
const ws = inject(websocket_key);
const available = ref(ws?.canBrowseFiles ?? false);
const pending = ref(false);
const error = ref("");
const lifetime = new AbortController();

function refreshAvailability() {
  available.value = ws?.canBrowseFiles ?? false;
}

async function browse() {
  if (!ws || !available.value || pending.value) return;
  pending.value = true;
  error.value = "";
  try {
    const response = await requestFileDialog(ws, props.node.id, lifetime.signal);
    if (response.action === action_e.error) {
      error.value = response.message || response.error;
    }
    // The server applies the path through configuration validation. Its normal
    // broadcast updates this field, including any corrected value.
  } catch (reason) {
    if (!lifetime.signal.aborted) {
      error.value = reason instanceof Error ? reason.message : "Could not complete file selection";
    }
  } finally {
    pending.value = false;
  }
}

ws?.on("on_connected", refreshAvailability);
ws?.on("on_disconnected", refreshAvailability);
onBeforeUnmount(() => {
  lifetime.abort();
  ws?.off("on_connected", refreshAvailability);
  ws?.off("on_disconnected", refreshAvailability);
});
</script>

<style scoped>
button {
  margin-bottom: 0.3em;
  color: #e0e0e0;
  background: #292943;
  border: 1px solid rgba(100, 100, 140, 0.5);
  border-radius: 3px;
  padding: 3px 6px;
  cursor: pointer;
}
button:disabled {
  cursor: wait;
  opacity: 0.65;
}
.file-error {
  color: #ffaaaa;
  font-size: 0.85em;
  white-space: normal;
}
</style>
