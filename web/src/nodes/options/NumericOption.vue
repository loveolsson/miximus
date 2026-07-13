<template>
  <div class="baklava-num-input">
    <div class="__button --dec" :title="`Decrease by ${intf.step}`" @click="adjust(-intf.step)">
      <svg class="baklava-icon" viewBox="0 0 24 24" aria-hidden="true">
        <polyline points="6 9 12 15 18 9" />
      </svg>
    </div>

    <div v-if="!editing" class="__content" @click="beginEdit">
      <div class="__label" :title="intf.name">{{ intf.name }}</div>
      <div class="__value">{{ displayValue }}</div>
    </div>
    <div v-else class="__content">
      <input
        ref="inputElement"
        v-model="editValue"
        type="number"
        :step="intf.step"
        :min="intf.min"
        :max="intf.max"
        class="baklava-input"
        :class="{ '--invalid': invalid }"
        style="text-align: right"
        @blur="commitEdit"
        @keydown.enter="commitEdit"
        @keydown.escape="cancelEdit"
      />
    </div>

    <div class="__button --inc" :title="`Increase by ${intf.step}`" @click="adjust(intf.step)">
      <svg class="baklava-icon" viewBox="0 0 24 24" aria-hidden="true">
        <polyline points="6 9 12 15 18 9" />
      </svg>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, ref } from "vue";
import type { NumberInterface } from "@baklavajs/renderer-vue";

type NumericInterface = NumberInterface & {
  precision: number;
  step: number;
  setNumericValue(value: number): void;
};

const props = defineProps<{
  intf: NumericInterface;
}>();

const editing = ref(false);
const invalid = ref(false);
const editValue = ref("");
const inputElement = ref<{ focus(): void }>();

function format(value: number): string {
  const fixed = value.toFixed(props.intf.precision);
  return fixed.includes(".") ? fixed.replace(/0+$/, "").replace(/\.$/, "") : fixed;
}

function round(value: number): number {
  const scale = 10 ** props.intf.precision;
  return Math.round((value + Number.EPSILON) * scale) / scale;
}

function setValue(value: number): boolean {
  const rounded = round(value);
  if (!Number.isFinite(rounded) || !props.intf.validate(rounded)) {
    return false;
  }
  props.intf.setNumericValue(rounded);
  return true;
}

const displayValue = computed(() => format(props.intf.value));

async function beginEdit() {
  invalid.value = false;
  editValue.value = format(props.intf.value);
  editing.value = true;
  await nextTick();
  inputElement.value?.focus();
}

function commitEdit() {
  if (!editing.value) {
    return;
  }

  if (setValue(Number.parseFloat(editValue.value))) {
    invalid.value = false;
    editing.value = false;
  } else {
    invalid.value = true;
  }
}

function cancelEdit() {
  invalid.value = false;
  editing.value = false;
}

function adjust(delta: number) {
  invalid.value = false;
  setValue(props.intf.value + delta);
}
</script>

<style scoped>
.baklava-icon {
  width: 16px;
  height: 16px;
  fill: none;
  stroke: currentColor;
  stroke-linecap: round;
  stroke-linejoin: round;
  stroke-width: 2;
}
</style>
