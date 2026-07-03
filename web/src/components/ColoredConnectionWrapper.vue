<script setup lang="ts">
/**
 * Replacement for BaklavaJS's built-in ConnectionWrapper that:
 *   1. Colors connections by interface type (restoring the v1 renderConnection
 *      hook behavior).
 *   2. Uses MutationObserver on the node's style attribute instead of Vue
 *      reactive watchers, so updateCoords() runs exactly when the node's
 *      CSS left/top has been committed — not before it.
 */
import { ref, computed, onMounted, onBeforeUnmount, nextTick } from "vue";
import {
  useGraph,
  useViewModel,
  getDomElements,
  getPortCoordinates,
} from "@baklavajs/renderer-vue";
import type { Connection } from "@baklavajs/core";
import { getType } from "@baklavajs/interface-types";
import { connectionColorMap } from "../nodes/interface_types";

// BaklavaJS adds isInDanger at runtime; it isn't in the core Connection type.
const props = defineProps<{ connection: Connection & { isInDanger?: boolean } }>();

const { graph } = useGraph();
const { viewModel } = useViewModel();

// --- raw port coords (graph-canvas space) ------------------------------------
const rawCoords = ref({ x1: 0, y1: 0, x2: 0, y2: 0 });

// --- state (danger highlight) ------------------------------------------------
const isForbidden = computed(() => props.connection.isInDanger === true);

// --- color -------------------------------------------------------------------
const strokeColor = computed(() => {
  const type = getType(props.connection.from) ?? getType(props.connection.to);
  return connectionColorMap.get(type ?? "") ?? "var(--baklava-color-connection-default)";
});

// --- SVG path (transforms graph-canvas coords → screen coords) ---------------
const d = computed(() => {
  const { x: px, y: py } = graph.value.panning;
  const s = graph.value.scaling;
  const { x1, y1, x2, y2 } = rawCoords.value;
  const tx1 = (x1 + px) * s;
  const ty1 = (y1 + py) * s;
  const tx2 = (x2 + px) * s;
  const ty2 = (y2 + py) * s;

  if (viewModel.value.settings.useStraightConnections) {
    return `M ${tx1} ${ty1} L ${tx2} ${ty2}`;
  }
  const dx = 0.3 * Math.abs(tx1 - tx2);
  return `M ${tx1} ${ty1} C ${tx1 + dx} ${ty1}, ${tx2 - dx} ${ty2}, ${tx2} ${ty2}`;
});

// --- DOM observers -----------------------------------------------------------
let resizeObserver: ResizeObserver | undefined;
let mutationObserver: MutationObserver | undefined;

const updateCoords = () => {
  const from = getDomElements(props.connection.from);
  const to = getDomElements(props.connection.to);
  if (from.node && to.node && !resizeObserver) {
    // ResizeObserver: fires when node dimensions change.
    resizeObserver = new ResizeObserver(updateCoords);
    resizeObserver.observe(from.node);
    resizeObserver.observe(to.node);
    // MutationObserver on the style attribute: Vue sets element.style.left/top
    // when node.position changes, which mutates the style attribute. This fires
    // after the DOM is updated, so offsetLeft/offsetTop are already correct.
    mutationObserver = new MutationObserver(updateCoords);
    mutationObserver.observe(from.node, { attributes: true, attributeFilter: ["style"] });
    mutationObserver.observe(to.node, { attributes: true, attributeFilter: ["style"] });
  }
  const [x1, y1] = getPortCoordinates(from);
  const [x2, y2] = getPortCoordinates(to);
  rawCoords.value = { x1, y1, x2, y2 };
};

onMounted(async () => {
  await nextTick();
  updateCoords();
});

onBeforeUnmount(() => {
  resizeObserver?.disconnect();
  mutationObserver?.disconnect();
});
</script>

<template>
  <path
    class="baklava-connection"
    :class="{ '--forbidden': isForbidden }"
    :d="d"
    :style="{ stroke: strokeColor }"
  />
</template>
