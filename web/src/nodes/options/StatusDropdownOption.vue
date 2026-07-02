<template>
  <div class="dark-input-option">
    <div class="__content">
      <div class="__label text-truncate">{{ name }}</div>
    </div>
    <div class="__content">
      <select class="status-select" v-model="localValue" @change="onUserChange">
        <option
          v-if="localValue && !listContainsCurrent"
          :value="localValue"
          class="stale-entry"
        >
          {{ localValue }} (unavailable)
        </option>
        <option v-if="!localValue" value="" disabled>— select —</option>
        <option v-for="item in availableList" :key="item" :value="item">
          {{ item }}
        </option>
      </select>
    </div>
  </div>
</template>

<script lang="ts">
import { Component, Prop, Vue, Watch } from "vue-property-decorator";
import { NodeOption } from "@baklavajs/core";
import { get_node_status } from "@/nodes/status_store";

@Component
export default class StatusDropdownOption extends Vue {
  @Prop({ type: String, default: "" })
  value!: string;

  @Prop({ type: String })
  name!: string;

  /** The full BaklavaJS option object — additionalProperties are merged in flat. */
  @Prop({ type: Object })
  option!: NodeOption;

  localValue = this.value;

  get availableList(): string[] {
    const node_id = this.option?.nodeData?.node_id ?? "";
    const status = get_node_status(node_id);
    const list = status[this.option?.list_key];
    return Array.isArray(list) ? (list as string[]) : [];
  }

  get listContainsCurrent(): boolean {
    return this.availableList.includes(this.localValue);
  }

  /**
   * Accept server-sent value (e.g. from initial config load) only when the
   * user has not yet made a local selection. Once the user picks a value it is
   * preserved even if the server sends a different one (e.g. device disconnect).
   */
  @Watch("value")
  onServerValueChanged(newValue: string, oldValue: string): void {
    if (this.localValue === oldValue) {
      this.localValue = newValue;
    }
  }

  onUserChange(): void {
    this.$emit("input", this.localValue);
  }
}
</script>

<style scoped>
.status-select {
  width: 100%;
  background-color: #1a1a2e;
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
