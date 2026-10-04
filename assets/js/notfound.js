// The 404 page, shared by every language: speaks the language of the address it was asked for.
(function () {
  "use strict";
  var data;
  try {
    data = JSON.parse(document.getElementById("notfound-data").textContent);
  } catch {
    return;
  }
  var has = function (k) {
    return typeof k === "string" && Object.prototype.hasOwnProperty.call(data, k);
  };
  var first = location.pathname.split("/")[1] || "";
  var lang = has(first) ? first : null;
  if (!lang) {
    try {
      var v = JSON.parse(window.localStorage.getItem("scacelith.lang"));
      if (has(v)) lang = v;
    } catch { /* none stored */ }
  }
  if (!lang) {
    var nav = String(navigator.language || "en").toLowerCase();
    lang = /^zh-(tw|hk|mo)/.test(nav) ? "zh-Hant" : /^zh/.test(nav) ? "zh-Hans" : (has(nav.split("-")[0]) ? nav.split("-")[0] : "en");
  }
  var m = data[lang];
  var html = document.documentElement;
  html.lang = lang;
  html.dir = m.dir;
  document.title = m.title + " · Scacelith";
  document.querySelector("[data-nf-title]").textContent = m.title;
  document.querySelector("[data-nf-text]").textContent = m.text;
  var home = document.querySelector("[data-nf-home]");
  home.textContent = m.home;
  home.setAttribute("href", "/" + lang + "/");
})();
