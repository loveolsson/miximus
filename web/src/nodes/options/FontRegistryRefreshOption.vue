<template>
  <div class="font-registry-option">
    <button type="button" :disabled="pending" @click="refreshFonts">
      {{ pending ? "Refreshing…" : "Refresh Fonts" }}
    </button>
    <span v-if="result" :class="resultClass">{{ result }}</span>
  </div>
</template>

<script setup lang="ts">
import { computed, inject, ref } from "vue";
import type { AbstractNode } from "@baklavajs/core";
import type { FontRegistryRefreshInterface } from "../interfaces";
import { action_e, topic_e, type command_font_registry_s } from "@/messages";
import { websocket_key } from "@/websocket";

defineProps<{
  modelValue: null;
  node: AbstractNode;
  intf: FontRegistryRefreshInterface;
}>();

const ws = inject(websocket_key);
const pending = ref(false);
const result = ref("");
const succeeded = ref(false);
const resultClass = computed(() => (succeeded.value ? "result--success" : "result--error"));

function refreshFonts(): void {
  if (!ws || pending.value) return;

  pending.value = true;
  result.value = "";

  const sent = ws.send<command_font_registry_s>(
    {
      action: action_e.command,
      topic: topic_e.font_registry,
      command: "refresh",
    },
    (response) => {
      pending.value = false;
      succeeded.value = response.action === action_e.result;
      result.value = succeeded.value ? "Updated" : "Failed";
    },
  );

  if (!sent) {
    pending.value = false;
    succeeded.value = false;
    result.value = "Offline";
  }
}
</script>

<style scoped>
.font-registry-option {
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
.result--success {
  color: #64b77a;
}
.result--error {
  color: #d26a6a;
}
</style>
