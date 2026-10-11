// The page Google sends the browser back to after "Sign in with Google" (/account/google/, the same
// for every language): keeps Google's answer for this tab, then replaces itself with the account
// page of the language the sign-in started from, so that the code stays neither in the history
// nor in the account page's address. The account page checks the answer (assets/js/account.js).
(function () {
  "use strict";
  var html = document.documentElement;
  var langs;
  try {
    langs = JSON.parse(html.getAttribute("data-langs"));
  } catch {
    langs = ["en"];
  }

  function read(area, key) {
    try {
      return JSON.parse(window[area].getItem(key));
    } catch {
      return null;
    }
  }

  // As the site root chooses (root.js): the language chosen before, else the browser's.
  function preferred() {
    var v = read("localStorage", "scacelith.lang");
    if (langs.indexOf(v) >= 0) return v;
    var wanted = (navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || "en"]);
    for (var i = 0; i < wanted.length; i++) {
      var w = String(wanted[i]).toLowerCase();
      if (/^zh-(tw|hk|mo)|^zh-hant/.test(w)) return "zh-Hant";
      if (/^zh/.test(w)) return "zh-Hans";
      var base = w.split("-")[0];
      if (langs.indexOf(base) >= 0) return base;
    }
    return null;
  }

  var attempt = read("sessionStorage", "scacelith.sso.attempt") || {};
  var query = new URLSearchParams(location.search);
  var answer = {};
  ["code", "state", "iss", "error"].forEach(function (name) {
    var v = query.get(name);
    if (v) answer[name] = v.slice(0, 4096);
  });
  if (answer.code || answer.error) {
    try {
      window.sessionStorage.setItem("scacelith.sso.return", JSON.stringify(answer));
    } catch { /* the account page then says that no sign-in was started in this tab */ }
  }
  var lang = langs.indexOf(attempt.lang) >= 0 ? attempt.lang : preferred() || "en";
  // The account page's own query ("?next=gif&id=..."), as it was when the sign-in started.
  var search = typeof attempt.search === "string" && /^(\?[^#]{0,2000})?$/.test(attempt.search) ? attempt.search : "";
  location.replace((html.getAttribute("data-root") || "/") + lang + "/account/" + search);
})();
