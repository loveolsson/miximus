<template>
  <div class="node-action-option">
    <button
      v-for="(action, index) in intf.actions"
      :key="index"
      type="button"
      :disabled="pending"
      @click="runAction(action)"
    >
      {{ action.label }}
    </button>
  </div>
</template>

<script setup lang="ts">
import { inject, onBeforeUnmount, ref } from "vue";
import type { AbstractNode } from "@baklavajs/core";
import type { NodeActionButton, NodeActionInterface } from "../interfaces";
import {
  action_e,
  topic_e,
  type node_action_request_s,
  type node_action_result_s,
} from "@/messages";
import { websocket_key } from "@/websocket";

const props = defineProps<{ modelValue: null; node: AbstractNode; intf: NodeActionInterface }>();
const ws = inject(websocket_key);
const pending = ref(false);
const lifetime = new AbortController();
onBeforeUnmount(() => lifetime.abort());

async function runAction(action: NodeActionButton): Promise<void> {
  if (!ws || pending.value) return;
  pending.value = true;
  try {
    const response = await ws.request<node_action_request_s, node_action_result_s>(
      {
        action: action_e.command,
        topic: topic_e.node_action,
        id: props.node.id,
        name: action.action,
        payload: action.payload === undefined ? {} : action.payload,
      },
      lifetime.signal,
    );
    if (lifetime.signal.aborted) return;
    if (response.action === action_e.error) {
      console.warn("Node action failed:", response.message || response.error);
    }
  } catch (error) {
    if (!lifetime.signal.aborted) console.warn("Node action failed:", error);
  } finally {
    if (!lifetime.signal.aborted) pending.value = false;
  }
}
</script>

<style scoped>
.node-action-option {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 0.3em;
}
button {
  flex: 1;
  color: #e0e0e0;
  background: #292943;
  border: 1px solid rgba(100, 100, 140, 0.5);
  border-radius: 3px;
  padding: 3px 6px;
  cursor: pointer;
}
button:hover:not(:disabled) {
  background: #353557;
}
button:disabled {
  cursor: wait;
  opacity: 0.65;
}
</style>
