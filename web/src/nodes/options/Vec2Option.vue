<template>
  <div class="dark-num-input">
    <div class="__name">{{ intf.name }}</div>
    <div class="__content">
      <label>x=</label>
      <input
        v-model="x"
        type="number"
        class="dark-input"
        style="text-align: right"
        @blur="doneEdit"
        @keydown.enter="doneEdit"
      />
    </div>
    <div class="__content">
      <label>y=</label>
      <input
        v-model="y"
        type="number"
        class="dark-input"
        style="text-align: right"
        @blur="doneEdit"
        @keydown.enter="doneEdit"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, watch } from "vue";
import type { NodeInterface } from "@baklavajs/core";
import type { AbstractNode } from "@baklavajs/core";

const props = defineProps<{
  modelValue: [number, number];
  node: AbstractNode;
  intf: NodeInterface<[number, number]>;
}>();

const emit = defineEmits<{
  (e: "update:modelValue", value: [number, number]): void;
}>();

const x = ref(props.modelValue[0]);
const y = ref(props.modelValue[1]);

watch(
  () => props.modelValue,
  (v) => {
    x.value = v[0];
    y.value = v[1];
  },
);

function doneEdit() {
  const nx = typeof x.value === "string" ? parseFloat(x.value) : x.value;
  const ny = typeof y.value === "string" ? parseFloat(y.value) : y.value;
  if (!Number.isNaN(nx) && !Number.isNaN(ny)) {
    emit("update:modelValue", [nx, ny]);
  }
}
</script>

<style scoped>
.dark-num-input {
  display: flex;
  flex-direction: column;
  width: 100%;
  gap: 2px;
}
.__name {
  color: #ccc;
  font-size: 0.85em;
  padding-bottom: 1px;
}
.__content {
  display: flex;
  align-items: center;
  gap: 4px;
  min-width: 0;
}
label {
  flex: 0 0 auto;
  color: #aaa;
  font-size: 0.8em;
  white-space: nowrap;
}
.dark-input {
  flex: 1;
  min-width: 0;
  width: 0; /* overridden by flex-grow; forces it to shrink properly */
  background: #1a1a2e;
  border: 1px solid rgba(100, 100, 140, 0.5);
  color: #e0e0e0;
  border-radius: 3px;
  padding: 2px 4px;
  font-size: 0.85em;
  box-sizing: border-box;
}
.dark-input:focus {
  outline: none;
  border-color: #5379b5;
}
</style>
