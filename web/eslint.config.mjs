import js from "@eslint/js";
import tseslint from "typescript-eslint";
import pluginVue from "eslint-plugin-vue";
import prettier from "eslint-config-prettier";

export default [
  js.configs.recommended,
  ...tseslint.configs.recommended,
  ...pluginVue.configs["flat/recommended"],
  prettier,
  {
    files: ["src/**/*.{ts,vue}"],
    languageOptions: {
      globals: {
        ResizeObserver: "readonly",
        MutationObserver: "readonly",
        setTimeout: "readonly",
        clearTimeout: "readonly",
        WebSocket: "readonly",
        console: "readonly",
      },
      parserOptions: {
        parser: tseslint.parser,
        extraFileExtensions: [".vue"],
      },
    },
    rules: {
      eqeqeq: "error",
      "no-console": "off",
      "@typescript-eslint/no-unused-vars": ["warn", { argsIgnorePattern: "^_" }],
      "vue/multi-word-component-names": "off",
    },
  },
];
