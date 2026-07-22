<template>
  <div class="node-status-indicator">
    <span
      ref="statusIcon"
      class="status-icon status-icon--interactive"
      tabindex="0"
      aria-label="Node status details"
      :aria-describedby="tooltipId"
      @mouseenter="showTooltip"
      @mouseleave="hideTooltip"
      @focus="showTooltip"
      @blur="hideTooltip"
    >
      <span class="dot" :class="dotClass"></span>
    </span>
    <span class="label">{{ statusLabel }}</span>
  </div>

  <Teleport to="body">
    <span
      v-if="tooltipVisible"
      :id="tooltipId"
      ref="statusTooltip"
      class="status-tooltip"
      role="tooltip"
      :style="tooltipPosition"
    >
      <span class="status-tooltip__title">Status</span>
      <span class="status-tooltip__sections">
        <span v-for="section in sections" :key="section.title" class="status-tooltip__section">
          <span class="status-tooltip__section-title">{{ section.title }}</span>
          <span v-for="detail in section.details" :key="detail.key" class="status-tooltip__row">
            <span class="status-tooltip__label">{{ detail.label }}</span>
            <span
              class="status-tooltip__value"
              :class="{ 'status-tooltip__value--missing': !detail.reported }"
            >
              {{ detail.value }}
            </span>
          </span>
        </span>
      </span>
    </span>
  </Teleport>
</template>

<script setup lang="ts">
import { computed, nextTick, reactive, ref } from "vue";
import type { AbstractNode } from "@baklavajs/core";
import type { NodeStatusField, NodeStatusInterface } from "../interfaces";
import { get_node_status, type NodeStatus } from "../status_store";

const props = defineProps<{
  modelValue: null;
  node: AbstractNode;
  intf: NodeStatusInterface;
}>();

interface PositionedElement {
  getBoundingClientRect(): {
    top: number;
    right: number;
    bottom: number;
    left: number;
    width: number;
    height: number;
  };
}

const statusIcon = ref<PositionedElement>();
const statusTooltip = ref<PositionedElement>();
const tooltipVisible = ref(false);
const tooltipPosition = reactive({ top: "0px", left: "0px" });
const tooltipId = `node-status-${props.intf.id}`;

interface StatusDetail {
  key: string;
  label: string;
  value: string;
  reported: boolean;
}

const status = computed<NodeStatus>(() => get_node_status(props.intf.nodeData.node_id));

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
  const format = status.value["active_format"];
  if (typeof format === "string" && format) parts.push(format);
  return parts.length > 0 ? parts.join(" · ") : "Waiting for status";
});

function formatValue(field: NodeStatusField, value: unknown): string {
  if (field.key === "connected") return value === true ? "Connected" : "Not connected";
  if (field.format === "locked") return value === true ? "Locked" : "Unlocked";
  if (field.format === "active") return value === true ? "Active" : "Idle";
  if (field.format === "busy") return value === true ? "Busy" : "Idle";
  if (field.format === "integer") {
    return typeof value === "number" ? value.toLocaleString() : String(value);
  }
  if (field.format === "temperature") {
    const temperature = typeof value === "number" ? value.toLocaleString() : String(value);
    return `${temperature} °C`;
  }
  return String(value);
}

const sections = computed(() =>
  props.intf.sections.map((section) => ({
    title: section.title,
    details: section.fields.map((field): StatusDetail => {
      if (!(field.key in status.value)) {
        return { key: field.key, label: field.label, value: "Waiting for status", reported: false };
      }

      const value = status.value[field.key];
      if (value === null || value === undefined) {
        return { key: field.key, label: field.label, value: "Unavailable", reported: false };
      }

      return {
        key: field.key,
        label: field.label,
        value: formatValue(field, value),
        reported: true,
      };
    }),
  })),
);

function positionTooltip(): void {
  if (!statusIcon.value || !statusTooltip.value) return;

  const margin = 8;
  const iconRect = statusIcon.value.getBoundingClientRect();
  const tooltipRect = statusTooltip.value.getBoundingClientRect();

  let left = iconRect.right + margin;
  if (left + tooltipRect.width > globalThis.innerWidth - margin) {
    left = iconRect.left - tooltipRect.width - margin;
  }

  const top = Math.min(
    Math.max(iconRect.top - margin, margin),
    globalThis.innerHeight - tooltipRect.height - margin,
  );

  tooltipPosition.left = `${Math.max(left, margin)}px`;
  tooltipPosition.top = `${Math.max(top, margin)}px`;
}

async function showTooltip(): Promise<void> {
  tooltipVisible.value = true;
  await nextTick();
  positionTooltip();
}

function hideTooltip(): void {
  tooltipVisible.value = false;
}
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

.status-icon {
  position: relative;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
  width: 11px;
  height: 11px;
  border-radius: 50%;
  outline: none;
}

.status-icon--interactive {
  cursor: help;
}

.status-icon--interactive:focus-visible {
  outline: 2px solid var(--baklava-control-color-primary);
  outline-offset: 2px;
}

.dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
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

.status-tooltip {
  position: fixed;
  z-index: 1000;
  display: grid;
  width: min(560px, calc(100vw - 16px));
  padding: 0.6em 0.75em;
  color: var(--baklava-node-interface-port-tooltip-color-foreground);
  background: var(--baklava-node-interface-port-tooltip-color-background);
  border-radius: var(--baklava-control-border-radius);
  box-shadow: 0 2px 8px rgb(0 0 0 / 55%);
  pointer-events: none;
  text-align: left;
  font-size: 1.05em;
  line-height: 1.35;
}

.status-tooltip__title {
  margin-bottom: 0.5em;
  color: var(--baklava-node-title-color-foreground);
  font-weight: 600;
}

.status-tooltip__sections {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0.8em 1.25em;
}

.status-tooltip__section {
  display: grid;
  grid-template-columns: max-content minmax(0, 1fr);
  align-content: start;
}

.status-tooltip__section-title {
  grid-column: 1 / -1;
  margin-bottom: 0.25em;
  color: var(--baklava-control-color-primary);
  font-weight: 600;
}

.status-tooltip__row {
  display: contents;
}

.status-tooltip__label {
  padding-right: 1.25em;
  color: var(--baklava-control-color-disabled-foreground);
  white-space: nowrap;
}

.status-tooltip__value {
  color: var(--baklava-control-color-foreground);
  overflow-wrap: anywhere;
}

.status-tooltip__value--missing {
  color: var(--baklava-control-color-disabled-foreground);
  font-style: italic;
}

@media (max-width: 700px) {
  .status-tooltip {
    width: min(320px, calc(100vw - 16px));
  }

  .status-tooltip__sections {
    grid-template-columns: 1fr;
  }
}
</style>
