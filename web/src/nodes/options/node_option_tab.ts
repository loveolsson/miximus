import type { ObjectDirective } from "vue";

const optionSelector = "[data-node-option-tab]";

function isAvailable(element: HTMLElement): boolean {
  return !element.matches(":disabled") && element.getClientRects().length > 0;
}

/**
 * Baklava cancels Tab at the editor boundary. Handle it on editable controls
 * first and deliberately cycle only through options in the current node.
 */
function handleTab(event: KeyboardEvent): void {
  if (event.key !== "Tab" || event.ctrlKey || event.altKey || event.metaKey) {
    return;
  }

  const current = event.currentTarget;
  if (!(current instanceof HTMLElement)) {
    return;
  }

  const node = current.closest(".baklava-node");
  if (!node) {
    return;
  }

  const options = Array.from(node.querySelectorAll<HTMLElement>(optionSelector)).filter(
    isAvailable,
  );
  const currentIndex = options.indexOf(current);
  if (currentIndex < 0 || options.length === 0) {
    return;
  }

  const offset = event.shiftKey ? -1 : 1;
  const nextIndex = (currentIndex + offset + options.length) % options.length;

  event.preventDefault();
  event.stopPropagation();
  options[nextIndex].focus();
}

export const vNodeOptionTab: ObjectDirective<HTMLElement> = {
  mounted(element) {
    element.dataset.nodeOptionTab = "";
    if (!element.hasAttribute("tabindex")) {
      element.tabIndex = 0;
    }
    element.addEventListener("keydown", handleTab);
  },
  beforeUnmount(element) {
    element.removeEventListener("keydown", handleTab);
  },
};
