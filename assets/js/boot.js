// Runs before the page is drawn: tells the stylesheet that scripts are available, and hides the
// page when another site shows it in a frame (GitHub Pages cannot send frame-ancestors).
document.documentElement.classList.replace("no-js", "js");
try {
  if (window.top !== window.self) document.documentElement.classList.add("is-framed");
} catch {
  document.documentElement.classList.add("is-framed");
}
