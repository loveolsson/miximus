<template>
  <div class="dark-num-input">
    <div class="__content">
      <div class="__label .text-truncate">{{ name }}</div>
    </div>
    <div class="__content">
      <label>x=</label>
      <input
        type="number"
        v-model="x"
        class="dark-input"
        ref="input"
        @blur="doneEdit"
        @keydown.enter="doneEdit"
        style="text-align: right"
      />
    </div>
    <div class="__content">
      <label>y=</label>
      <input
        type="number"
        v-model="y"
        class="dark-input"
        ref="input"
        @blur="doneEdit"
        @keydown.enter="doneEdit"
        style="text-align: right"
      />
    </div>
  </div>
</template>

<script lang="ts">
import { Component, Prop, Vue, Watch } from "vue-property-decorator";
@Component
export default class Vec2Option extends Vue {
  @Prop({ type: Array })
  value!: [number, number];

  @Prop({ type: String })
  name!: string;

  x = this.value[0];
  y = this.value[1];

  @Watch("value")
  setValue(newValue: [number, number]) {
    this.x = newValue[0];
    this.y = newValue[1];
  }

  doneEdit() {
    console.log(this);
    const nx = typeof this.x === "string" ? parseFloat(this.x) : this.x;
    const ny = typeof this.y === "string" ? parseFloat(this.y) : this.y;

    if (!Number.isNaN(nx) && !Number.isNaN(ny)) {
      this.$emit("input", [nx, ny]);
    }
  }
}
</script>
