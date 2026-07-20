<template>
  <div class="dark-input-option">
    <div class="__content">
      <div class="__label text-truncate">{{ intf.name }}</div>
    </div>
    <div class="__content">
      <select v-model="localValue" v-node-option-tab class="dark-select">
        <option v-for="item in intf.items" :key="item" :value="item">
          {{ item }}
        </option>
      </select>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from "vue";
import type { AbstractNode } from "@baklavajs/core";
import type { DropdownInterface } from "../interfaces";
import { vNodeOptionTab } from "./node_option_tab";

const props = defineProps<{
  modelValue: string;
  node: AbstractNode;
  intf: DropdownInterface;
}>();

const emit = defineEmits<{
  (event: "update:modelValue", value: string): void;
}>();

const localValue = computed({
  get: () => props.modelValue,
  set: (value: string) => emit("update:modelValue", value),
});
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
.dark-select {
  width: 100%;
  background: #1a1a2e;
  color: #e0e0e0;
  border: 1px solid rgba(100, 100, 140, 0.5);
  border-radius: 3px;
  padding: 2px 4px;
  font-size: 0.85em;
  cursor: pointer;
}
.dark-select:focus {
  outline: none;
  border-color: #5379b5;
}
</style>
