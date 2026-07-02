<template>
  <div class="node-status-indicator" v-if="hasStatus">
    <span class="dot" :class="dotClass"></span>
    <span class="label">{{ statusLabel }}</span>
  </div>
</template>

<script lang="ts">
import { Component, Prop, Vue } from "vue-property-decorator";
import { NodeOption } from "@baklavajs/core";
import { get_node_status } from "@/nodes/status_store";

@Component
export default class NodeStatusIndicator extends Vue {
  /** The full BaklavaJS option object — additionalProperties are merged in flat. */
  @Prop({ type: Object })
  option!: NodeOption;

  get status(): Record<string, unknown> {
    const node_id = this.option?.nodeData?.node_id ?? "";
    return get_node_status(node_id);
  }

  get hasStatus(): boolean {
    return Object.keys(this.status).length > 0;
  }

  get isConnected(): boolean | null {
    if ("connected" in this.status) {
      return this.status["connected"] as boolean;
    }
    return null;
  }

  get dotClass(): string {
    if (this.isConnected === true) return "dot--connected";
    if (this.isConnected === false) return "dot--disconnected";
    return "dot--unknown";
  }

  get statusLabel(): string {
    const parts: string[] = [];

    if (this.isConnected === true) parts.push("Connected");
    else if (this.isConnected === false) parts.push("Not connected");

    const fmt = this.status["active_format"];
    if (typeof fmt === "string" && fmt) {
      parts.push(fmt);
    }

    return parts.join(" · ");
  }
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

.dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  flex-shrink: 0;
}

.dot--connected {
  background-color: #44aa66;
  box-shadow: 0 0 4px #44aa66;
}

.dot--disconnected {
  background-color: #cc4444;
}

.dot--unknown {
  background-color: #666;
}
</style>
