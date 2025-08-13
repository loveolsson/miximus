<template>
  <div class="dark-input-option">
    <div class="__content">
      <div class="__label text-truncate">{{ name }}</div>
    </div>
    <div class="__content">
      <input
        type="number"
        v-model="localValue"
        class="dark-input"
        @focus="onFocus"
        @blur="onBlur"
        @keydown.enter="onBlur"
        style="text-align: right"
      />
    </div>
  </div>
</template>

<script lang="ts">
import { Component, Prop, Vue, Watch } from "vue-property-decorator";

@Component
export default class FocusTrackingNumberOption extends Vue {
  @Prop({ type: Number })
  value!: number;

  @Prop({ type: String })
  name!: string;

  localValue = this.value;
  isFocused = false;
  latestServerValue = this.value;

  @Watch("value")
  setValue(newValue: number) {
    console.log(`[${this.name}] Server value changed: ${newValue}, focused: ${this.isFocused}`);
    
    // Always store the latest server value
    this.latestServerValue = newValue;
    
    if (!this.isFocused) {
      // Only update local value when not focused
      console.log(`[${this.name}] Applying server value: ${newValue}`);
      this.localValue = newValue;
    } else {
      // Block server updates while focused
      console.log(`[${this.name}] Blocking server update while focused, stored for later`);
    }
  }

  @Watch("localValue")
  onLocalValueChange(newValue: number | string) {
    // Only emit if we're focused (user is editing)
    if (this.isFocused) {
      const numValue = typeof newValue === "string" ? parseFloat(newValue) : newValue;
      if (!Number.isNaN(numValue)) {
        console.log(`[${this.name}] User input: ${numValue}`);
        this.$emit("input", numValue);
      }
    }
  }

  onFocus() {
    console.log(`[${this.name}] Focus gained - blocking server updates`);
    this.isFocused = true;
    // Store the current server value as the baseline
    this.latestServerValue = this.value;
  }

  onBlur() {
    console.log(`[${this.name}] Focus lost - sending final value and applying server state`);
    this.isFocused = false;
    
    // Send the final user value first
    const numValue = typeof this.localValue === "string" ? parseFloat(this.localValue) : this.localValue;
    if (!Number.isNaN(numValue)) {
      console.log(`[${this.name}] Emitting final value: ${numValue}`);
      this.$emit("input", numValue);
    }
    
    // Apply any server value that arrived while focused - immediately
    if (this.latestServerValue !== this.localValue) {
      console.log(`[${this.name}] Applying server value immediately: ${this.latestServerValue}`);
      this.localValue = this.latestServerValue;
    }
  }
}
</script>

<style scoped>
.dark-input-option {
  display: flex;
  align-items: center;
  margin-bottom: 0.5em;
}

.__content {
  flex: 1;
  margin-right: 0.5em;
}

.__content:last-child {
  margin-right: 0;
}

.__label {
  color: #ffffff;
  font-size: 0.9em;
  margin-bottom: 0.2em;
}

.dark-input {
  background-color: #2a2a2a;
  border: 1px solid #444;
  color: #ffffff;
  padding: 0.3em;
  border-radius: 3px;
  width: 100%;
}

.dark-input:focus {
  outline: none;
  border-color: #5379b5;
}
</style>
