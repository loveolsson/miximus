<template>
  <div class="dark-input-option">
    <div class="__content">
      <div class="__label text-truncate">{{ intf.name }}</div>
    </div>
    <div class="__content">
      <input
        v-model="localValue"
        type="number"
        class="dark-input"
        style="text-align: right"
        @focus="onFocus"
        @blur="onBlur"
        @keydown.enter="onBlur"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, watch } from "vue";
import type { NodeInterface } from "@baklavajs/core";
import type { AbstractNode } from "@baklavajs/core";

const props = defineProps<{
  modelValue: number;
  node: AbstractNode;
  intf: NodeInterface<number>;
}>();

const emit = defineEmits<{
  (e: "update:modelValue", value: number): void;
}>();

const localValue = ref(props.modelValue);
const isFocused = ref(false);
const latestServerValue = ref(props.modelValue);

watch(
  () => props.modelValue,
  (newValue) => {
    latestServerValue.value = newValue;
    if (!isFocused.value) {
      localValue.value = newValue;
    }
  },
);

function onFocus() {
  isFocused.value = true;
  latestServerValue.value = props.modelValue;
}

function onBlur() {
  isFocused.value = false;
  const num =
    typeof localValue.value === "string" ? parseFloat(localValue.value) : localValue.value;
  if (!Number.isNaN(num)) {
    emit("update:modelValue", num);
  }
  if (latestServerValue.value !== localValue.value) {
    localValue.value = latestServerValue.value;
  }
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
.dark-input {
  flex: 1;
  background: #1a1a2e;
  border: 1px solid rgba(100, 100, 140, 0.5);
  color: #e0e0e0;
  border-radius: 3px;
  padding: 2px 4px;
  font-size: 0.85em;
  width: 100%;
}
.dark-input:focus {
  outline: none;
  border-color: #5379b5;
}
</style>
