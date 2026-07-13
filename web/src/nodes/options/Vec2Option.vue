<template>
  <div class="vec2-input">
    <div class="vec2-input__name text-truncate" :title="intf.name">{{ intf.name }}</div>
    <NumericComponent :intf="xInterface" />
    <NumericComponent :intf="yInterface" />
  </div>
</template>

<script setup lang="ts">
import { shallowReactive, watch } from "vue";
import type { AbstractNode, NodeInterface } from "@baklavajs/core";
import NumericComponent from "./NumericOption.vue";
import { NumericInterface, type NumericOptions } from "../numeric";

type Vec2NodeInterface = NodeInterface<[number, number]> & {
  numericOptions: NumericOptions;
};

const props = defineProps<{
  modelValue: [number, number];
  node: AbstractNode;
  intf: Vec2NodeInterface;
}>();

const emit = defineEmits<{
  (e: "update:modelValue", value: [number, number]): void;
}>();

const xInterface = shallowReactive(
  new NumericInterface("x", props.modelValue[0], props.intf.numericOptions),
);
const yInterface = shallowReactive(
  new NumericInterface("y", props.modelValue[1], props.intf.numericOptions),
);
let syncingModel = false;

watch(
  () => props.modelValue,
  ([x, y]) => {
    syncingModel = true;
    xInterface.value = x;
    yInterface.value = y;
    syncingModel = false;
  },
  { flush: "sync" },
);

watch(
  [() => xInterface.value, () => yInterface.value],
  ([x, y]) => {
    if (!syncingModel && (x !== props.modelValue[0] || y !== props.modelValue[1])) {
      emit("update:modelValue", [x, y]);
    }
  },
  { flush: "sync" },
);
</script>

<style scoped>
.vec2-input {
  display: grid;
  width: 100%;
  gap: 0.25em;
  min-width: 0;
}

.vec2-input__name {
  color: var(--baklava-node-color-foreground);
  font-size: 0.85em;
}
</style>
