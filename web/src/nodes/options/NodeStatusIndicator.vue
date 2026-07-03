<template>
  <div v-if="hasStatus" class="node-status-indicator">
    <span class="dot" :class="dotClass"></span>
    <span class="label">{{ statusLabel }}</span>
  </div>
</template>

<script setup lang="ts">
import { computed } from "vue";
import type { AbstractNode } from "@baklavajs/core";
import type { NodeStatusInterface } from "../interfaces";
import { get_node_status, type NodeStatus } from "../status_store";

const props = defineProps<{
  modelValue: null;
  node: AbstractNode;
  intf: NodeStatusInterface;
}>();

const status = computed<NodeStatus>(() => get_node_status(props.intf.nodeData.node_id));

const hasStatus = computed(() => Object.keys(status.value).length > 0);

const isConnected = computed((): boolean | null => {
  if ("connected" in status.value) return status.value["connected"] as boolean;
  return null;
});

const dotClass = computed(() => {
  if (isConnected.value === true) return "dot--connected";
  if (isConnected.value === false) return "dot--disconnected";
  return "dot--unknown";
});

const statusLabel = computed(() => {
  const parts: string[] = [];
  if (isConnected.value === true) parts.push("Connected");
  else if (isConnected.value === false) parts.push("Not connected");
  const fmt = status.value["active_format"];
  if (typeof fmt === "string" && fmt) parts.push(fmt);
  return parts.join(" · ");
});
</script>

<style scoped>
.node-status-indicator {
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 2px 0;
  font-size: 0.8em;
  color: #a0a0b0;
}
.dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  flex-shrink: 0;
}
.dot--connected {
  background: #44aa66;
  box-shadow: 0 0 4px #44aa66;
}
.dot--disconnected {
  background: #cc4444;
}
.dot--unknown {
  background: #666;
}
</style>
