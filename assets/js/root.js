// The site root: sends the visitor to the language they chose before, or to the first of their
// browser's languages that the site speaks. "/#choose" shows the list instead.
(function () {
  "use strict";
  var LANGS = ["en", "fr", "de", "es", "uk", "ru", "ar", "ja", "zh-Hans", "zh-Hant"];

  function stored() {
    try {
      var v = JSON.parse(window.localStorage.getItem("scacelith.lang"));
      return LANGS.indexOf(v) >= 0 ? v : null;
    } catch (e) {
      return null;
    }
  }

  function preferred() {
    var wanted = (navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || "en"]);
    for (var i = 0; i < wanted.length; i++) {
      var w = String(wanted[i]).toLowerCase();
      if (/^zh-(tw|hk|mo)|^zh-hant/.test(w)) return "zh-Hant";
      if (/^zh/.test(w)) return "zh-Hans";
      var base = w.split("-")[0];
      if (LANGS.indexOf(base) >= 0) return base;
    }
    return null;
  }

  var choose = /(^|[#&?])choose\b/.test(location.hash + location.search);
  var lang = stored() || preferred();
  if (!choose) {
    location.replace((lang || "en") + "/" + location.hash);
    return;
  }
  document.addEventListener("DOMContentLoaded", function () {
    var suggestion = lang || "en";
    var links = document.querySelectorAll(".chooser-list a[data-lang]");
    for (var i = 0; i < links.length; i++) {
      var a = links[i];
      if (a.getAttribute("data-lang") === suggestion) a.className += " is-suggested";
      a.addEventListener("click", function (event) {
        try {
          window.localStorage.setItem("scacelith.lang", JSON.stringify(event.currentTarget.getAttribute("data-lang")));
        } catch (e) { /* not stored */ }
      });
    }
  });
})();
