<template>
  <div class="dark-input-option">
    <div class="__content">
      <div class="__label text-truncate">{{ intf.name }}</div>
    </div>
    <div class="__content">
      <select v-model="localValue" class="status-select" @change="onUserChange">
        <option v-if="localValue && !listContainsCurrent" :value="localValue" class="stale-entry">
          {{ localValue }} (unavailable)
        </option>
        <option v-if="!localValue" value="" disabled>— select —</option>
        <option v-for="item in availableList" :key="item.id" :value="item.id">
          {{ item.label }}
        </option>
      </select>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch } from "vue";
import type { AbstractNode } from "@baklavajs/core";
import type { SettingsOption, StatusDropdownInterface } from "../interfaces";
import { get_node_status } from "../status_store";

const props = defineProps<{
  modelValue: string;
  node: AbstractNode;
  intf: StatusDropdownInterface;
}>();

const emit = defineEmits<{
  (e: "update:modelValue", value: string): void;
}>();

const localValue = ref(props.modelValue);

const availableList = computed((): SettingsOption[] => {
  const status = get_node_status(props.intf.nodeData.node_id);
  const list = status[props.intf.list_key];
  if (!Array.isArray(list)) {
    return [];
  }
  return list.filter(
    (item): item is SettingsOption =>
      typeof item === "object" &&
      item !== null &&
      typeof (item as Record<string, unknown>).id === "string" &&
      typeof (item as Record<string, unknown>).label === "string",
  );
});

const listContainsCurrent = computed(() =>
  availableList.value.some((item) => item.id === localValue.value),
);

// Accept server value only while user hasn't deviated from it.
watch(
  () => props.modelValue,
  (newVal, oldVal) => {
    if (localValue.value === oldVal) {
      localValue.value = newVal;
    }
  },
);

function onUserChange() {
  emit("update:modelValue", localValue.value);
}
</script>

<style scoped>
.dark-input-option {
  display: flex;
  flex-direction: column;
  margin-bottom: 0.3em;
}
.__content {
  display: flex;
  align-items: center;
}
.__label {
  color: #ccc;
  font-size: 0.85em;
  padding: 2px 0;
}
.status-select {
  width: 100%;
  background: #1a1a2e;
  color: #e0e0e0;
  border: 1px solid rgba(100, 100, 140, 0.5);
  border-radius: 3px;
  padding: 2px 4px;
  font-size: 0.85em;
  cursor: pointer;
}
.status-select:focus {
  outline: none;
  border-color: #5379b5;
}
.stale-entry {
  color: #a07040;
  font-style: italic;
}
</style>
