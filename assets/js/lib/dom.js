// Small DOM helpers. Text always goes in as text nodes: nothing from the server is parsed as HTML.

export const $ = (selector, root = document) => root.querySelector(selector);
export const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

const SVG = "http://www.w3.org/2000/svg";

/**
 * h("a", {class: "x", href, dataset: {id: 1}, onclick}, "text", child, [more])
 * Attributes set to false, null or undefined are left out; true sets an empty attribute.
 */
export function h(tag, attrs, ...children) {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [key, value] of Object.entries(attrs)) {
      if (value === false || value === null || value === undefined) continue;
      if (key === "class") el.className = value;
      else if (key === "dataset") Object.assign(el.dataset, value);
      else if (key === "text") el.textContent = value;
      else if (key.startsWith("on") && typeof value === "function") el.addEventListener(key.slice(2), value);
      else el.setAttribute(key, value === true ? "" : String(value));
    }
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue;
    if (Array.isArray(child)) append(el, child);
    else el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

/** An icon of the page's sprite (templates/partials/icons.html). */
export function icon(name, className = "") {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("class", `icon ${className}`.trim());
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("focusable", "false");
  const use = document.createElementNS(SVG, "use");
  use.setAttribute("href", `#i-${name}`);
  svg.append(use);
  return svg;
}

export function clear(el) {
  el.replaceChildren();
  return el;
}

/** Shows exactly one of the elements matching `[data-view]` inside root. */
export function showView(root, name) {
  for (const el of $$("[data-view]", root)) el.hidden = el.dataset.view !== name;
}
