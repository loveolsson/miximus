// Allow importing .vue components. Volar generates precise per-component types
// that take precedence over this wildcard; it serves as a fallback only.
declare module "*.vue" {
  import type { DefineComponent } from "vue";
  const component: DefineComponent;
  export default component;
}

// CSS side-effect imports (e.g. @baklavajs/themes)
declare module "*.css" {
  const styles: Record<string, string>;
  export default styles;
}
