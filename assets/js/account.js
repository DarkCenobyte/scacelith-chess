// The account page: signing in (with two-factor authentication, or with Google), creating an
// account (with the server's proof of work), password recovery, and once signed in the player's
// information, ratings, game history, devices and settings.

import "./site.js";
import { onReady } from "./site.js";
import { $, $$, h, icon, clear, showView } from "./lib/dom.js";
import { t, fmt, pageUrl, ltr, lang } from "./lib/i18n.js";
import { api, withPow, ApiError, fileNameOf } from "./lib/api.js";
import { getSession, setSession, clearSession, updateUser, onSessionChange, requestSession } from "./lib/session.js";
import { toast, errorMessage, showMessage, setBusy, isBusy, initTabs, initReveal, saveBlob, fieldError } from "./lib/ui.js";
import * as store from "./lib/store.js";
import { renderRatings } from "./lib/ratings.js";
import { gameList } from "./lib/games.js";

const root = () => $(".account-body");
let info = null;
let infoPromise = null;
let me = null;
let view = null;
let signingIn = false;
let signingOut = false;

function signOutHere() {
  signingOut = true;
  clearSession();
}

const limits = () => ({
  usernameMin: 3,
  usernameMax: 20,
  usernamePattern: "^[A-Za-z0-9][A-Za-z0-9_-]*$",
  passwordMinLength: 10,
  passwordMaxBytes: 256,
  ...(info?.limits || {}),
});

function loadInfo() {
  infoPromise ||= api("/info", { timeout: 15000 }).then((i) => {
    info = i;
    for (const el of $$("[data-server-name]")) if (i.name) el.textContent = i.name;
    const l = limits();
    for (const hint of $$("[data-username-hint]")) hint.textContent = t("js.errors.username_format", { min: l.usernameMin, max: l.usernameMax });
    const pwHint = $("[data-password-hint]");
    if (pwHint) pwHint.textContent = t("js.account.password_rules", { min: l.passwordMinLength });
    if (i.registration === "closed") {
      $("[data-registration-closed]").hidden = false;
      $('[data-form="register"] [data-submit]').disabled = true;
    }
    if (i.sso?.googleWeb === true) {
      for (const el of $$("[data-google]")) el.hidden = false;
      $("[data-auth-foot]").textContent = t("js.account.auth_foot_google");
    }
    return i;
  }).catch((e) => {
    infoPromise = null;
    throw e;
  });
  return infoPromise;
}

function browserName() {
  const brands = navigator.userAgentData?.brands?.map((b) => b.brand) || [];
  const ua = navigator.userAgent || "";
  const browser =
    brands.find((b) => /Edge|Opera|Brave|Vivaldi/.test(b)) ||
    (/Firefox\//.test(ua) && "Firefox") || (/Edg\//.test(ua) && "Edge") || (/OPR\//.test(ua) && "Opera") ||
    (/Chrome\//.test(ua) && "Chrome") || (/Safari\//.test(ua) && "Safari") || brands.find((b) => !/Not.?A.?Brand|Chromium/.test(b)) || "";
  const os = /Windows/.test(ua) ? "Windows" : /Android/.test(ua) ? "Android" : /iPhone|iPad/.test(ua) ? "iOS"
    : /Mac OS X/.test(ua) ? "macOS" : /Linux|X11/.test(ua) ? "Linux" : "";
  return [browser, os].filter(Boolean).join(", ") || "web";
}

const clientLabel = () => t("js.account.client_label", { browser: browserName() }).slice(0, 64);

function formParts(form) {
  return { error: $("[data-error]", form), success: $("[data-success]", form), submit: $("[data-submit]", form) };
}

function resetMessages(form) {
  const { error, success } = formParts(form);
  showMessage(error, "");
  showMessage(success, "");
  for (const input of $$("input", form)) fieldError(input, null);
}

function fail(form, message, input) {
  const { error } = formParts(form);
  showMessage(error, message);
  if (input) {
    fieldError(input, message);
    input.focus();
  }
}

const value = (form, name) => (form.elements[name]?.value ?? "").trim();
const raw = (form, name) => form.elements[name]?.value ?? "";
const utf8Length = (s) => new TextEncoder().encode(s).length;
const looksLikeEmail = (s) => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(s);

function usernameOk(username, l) {
  let pattern;
  try {
    pattern = new RegExp(l.usernamePattern);
  } catch {
    pattern = /^[A-Za-z0-9][A-Za-z0-9_-]*$/;
  }
  return username.length >= l.usernameMin && username.length <= l.usernameMax && pattern.test(username);
}

/* ============================================================================ signed out */

let selectAuthTab = null;
let mfaToken = null;
let mfaRemember = false;

function showAuthPanel(name, { focus = true } = {}) {
  const panels = ["signin", "register", "forgot", "resend"];
  if (!panels.includes(name)) name = "signin";
  if (name === "signin" || name === "register") {
    selectAuthTab(name);
  } else {
    for (const tab of $$("[data-auth-tabs] [role=tab]")) {
      tab.setAttribute("aria-selected", "false");
      tab.tabIndex = tab.dataset.tab === "signin" ? 0 : -1;
    }
    for (const p of panels) $(`[data-panel="${p}"]`).hidden = p !== name;
  }
  if (name === "signin") showSigninStep("signin");
  const hash = name === "signin" ? "" : `#${name}`;
  if (location.hash !== hash) history.replaceState(null, "", hash || location.pathname + location.search);
  if (focus) $(`[data-panel="${name}"] input:not([type=checkbox])`)?.focus();
}

/** Shows one step of the sign-in panel: the password, the second factor, or a step of Google sign-in. */
function showSigninStep(name) {
  for (const form of $$('[data-panel="signin"] form')) form.hidden = form.dataset.form !== name;
  return $(`[data-form="${name}"]`);
}

function setMfaMode(recovery) {
  $("[data-mfa-label]").textContent = t(recovery ? "js.account.mfa_recovery" : "js.account.mfa_app");
  $("[data-mfa-text]").textContent = t(recovery ? "js.account.mfa_recovery_text" : "js.account.mfa_app_text");
  const input = $('[data-form="mfa"]').elements.code;
  input.inputMode = recovery ? "text" : "numeric";
  input.autocomplete = recovery ? "off" : "one-time-code";
  input.value = "";
}

/** The second factor, after a password or a Google sign-in. */
function askMfa(token, remember) {
  mfaToken = token;
  mfaRemember = remember;
  const mfa = showSigninStep("mfa");
  resetMessages(mfa);
  mfa.reset();
  setMfaMode(false);
  mfa.elements.code.focus();
}

function initAuth() {
  selectAuthTab = initTabs($("[data-auth-tabs]"), (name) => $(`[data-panel="${name}"]`), (name) => {
    for (const p of ["forgot", "resend"]) $(`[data-panel="${p}"]`).hidden = true;
    if (name === "signin") {
      // A finished sign-up gives way to the form again the next time.
      $("[data-register-done]").hidden = true;
      $('[data-form="register"]').hidden = false;
    }
    const hash = name === "signin" ? "" : `#${name}`;
    if (location.hash !== hash) history.replaceState(null, "", hash || location.pathname + location.search);
  });
  for (const link of $$("[data-goto]")) {
    link.addEventListener("click", (event) => {
      event.preventDefault();
      const target = link.dataset.goto;
      const login = value($('[data-form="signin"]'), "login");
      if ((target === "forgot" || target === "resend") && looksLikeEmail(login)) {
        $(`[data-form="${target}"] [name=email]`).value = login;
      }
      if (target === "signin") {
        $("[data-register-done]").hidden = true;
        $('[data-form="register"]').hidden = false;
      }
      showAuthPanel(target);
    });
  }
  initReveal(root());

  /* sign in */
  const signin = $('[data-form="signin"]');
  signin.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(signin).submit)) return;
    resetMessages(signin);
    const login = value(signin, "login");
    const password = raw(signin, "password");
    if (!login) return fail(signin, t("js.errors.required"), signin.elements.login);
    if (!password) return fail(signin, t("js.errors.required"), signin.elements.password);
    const { submit } = formParts(signin);
    setBusy(submit, true);
    signingIn = true;
    try {
      const body = { login, password, clientLabel: clientLabel() };
      const res = await withPow((pow) => api("/auth/login", { method: "POST", body: pow ? { ...body, pow } : body }));
      mfaRemember = signin.elements.remember.checked;
      if (res.mfaRequired) {
        askMfa(res.mfaToken, mfaRemember);
      } else {
        completeSignIn(res, mfaRemember);
        signin.reset();
      }
    } catch (e) {
      let message = errorMessage(e);
      if (e instanceof ApiError && e.code === "email_unverified" && looksLikeEmail(login)) {
        $('[data-form="resend"] [name=email]').value = login;
      }
      fail(signin, message, e instanceof ApiError && e.code === "invalid_credentials" ? signin.elements.password : null);
    } finally {
      signingIn = false;
      setBusy(submit, false);
    }
  });

  /* second factor */
  const mfa = $('[data-form="mfa"]');
  $("[data-mfa-recovery]").addEventListener("change", (event) => {
    setMfaMode(event.target.checked);
    mfa.elements.code.focus();
  });
  mfa.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(mfa).submit)) return;
    resetMessages(mfa);
    const code = value(mfa, "code");
    if (!code) return fail(mfa, t("js.errors.required"), mfa.elements.code);
    const recovery = mfa.elements.recovery.checked;
    const { submit } = formParts(mfa);
    setBusy(submit, true);
    signingIn = true;
    try {
      const res = await api("/auth/login/mfa", { method: "POST", body: recovery ? { mfaToken, recoveryCode: code } : { mfaToken, code: code.replace(/\s+/g, "") } });
      mfaToken = null;
      completeSignIn(res, mfaRemember);
      mfa.reset();
    } catch (e) {
      if (e instanceof ApiError && e.code === "invalid_mfa_token") {
        showAuthPanel("signin");
        fail($('[data-form="signin"]'), errorMessage(e));
      } else {
        fail(mfa, errorMessage(e), mfa.elements.code);
      }
    } finally {
      signingIn = false;
      setBusy(submit, false);
    }
  });

  /* create an account */
  const register = $('[data-form="register"]');
  register.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(register).submit)) return;
    resetMessages(register);
    await loadInfo().catch(() => null);
    const l = limits();
    const username = value(register, "username");
    const email = value(register, "email");
    const password = raw(register, "password");
    const confirm = raw(register, "confirm");
    if (!usernameOk(username, l)) {
      return fail(register, t("js.errors.username_format", { min: l.usernameMin, max: l.usernameMax }), register.elements.username);
    }
    if (!looksLikeEmail(email)) return fail(register, t("js.errors.email_format"), register.elements.email);
    if ([...password].length < l.passwordMinLength) return fail(register, t("js.errors.password_length", { min: l.passwordMinLength }), register.elements.password);
    if (utf8Length(password) > l.passwordMaxBytes) return fail(register, t("js.errors.weak_too_long"), register.elements.password);
    if (password !== confirm) return fail(register, t("js.errors.passwords_differ"), register.elements.confirm);

    const { submit } = formParts(register);
    const progress = $("[data-pow-progress]");
    setBusy(submit, true);
    try {
      const body = { username, email, password };
      const res = await withPow(
        (pow) => api("/auth/register", { method: "POST", body: pow ? { ...body, pow } : body, as: "response" }),
        (solving) => {
          progress.hidden = !solving;
          submit.setAttribute("aria-label", solving ? t("js.account.pow_solving") : "");
          if (!solving) submit.removeAttribute("aria-label");
        },
      );
      const data = await res.json().catch(() => ({}));
      register.reset();
      if (res.status === 201 || data.status === "ready") {
        showAuthPanel("signin", { focus: false });
        $('[data-form="signin"] [name=login]').value = username;
        showMessage($('[data-form="signin"] [data-error]'), "");
        toast(t("js.account.register_ready"), { type: "ok" });
        $('[data-form="signin"] [name=password]').focus();
      } else {
        $("[data-register-done-text]").textContent = t("js.account.register_done", { email });
        register.hidden = true;
        const done = $("[data-register-done]");
        done.hidden = false;
        done.focus();
        $('[data-form="resend"] [name=email]').value = email;
      }
    } catch (e) {
      const fieldFor = { invalid_username: "username", username_taken: "username", invalid_email: "email", email_taken: "email", weak_password: "password" };
      const name = e instanceof ApiError ? fieldFor[e.code] || e.field : null;
      fail(register, errorMessage(e, { passwordMin: l.passwordMinLength }), name ? register.elements[name] : null);
    } finally {
      progress.hidden = true;
      setBusy(submit, false);
    }
  });

  /* password reset link, confirmation e-mail again */
  for (const [name, path, doneKey] of [["forgot", "/auth/password/forgot", "js.account.forgot_done"], ["resend", "/auth/verify-email/resend", "js.account.resend_done"]]) {
    const form = $(`[data-form="${name}"]`);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (isBusy(formParts(form).submit)) return;
      resetMessages(form);
      const email = value(form, "email");
      if (!looksLikeEmail(email)) return fail(form, t("js.errors.email_format"), form.elements.email);
      const { submit, success } = formParts(form);
      setBusy(submit, true);
      try {
        await api(path, { method: "POST", body: { email } });
        showMessage(success, t(doneKey));
      } catch (e) {
        fail(form, errorMessage(e));
      } finally {
        setBusy(submit, false);
      }
    });
  }

  initGoogle();
}

// Where the visitor was going before signing in: a dashboard tab (#games) or the GIF page.
const entry = new URLSearchParams(location.search);
let pendingTab = ["games", "devices", "settings"].includes(location.hash.slice(1)) ? location.hash.slice(1) : null;

function completeSignIn(res, remember) {
  setSession({ token: res.token, expiresAt: res.expiresAt, user: res.user }, remember);
  if (entry.get("next") === "gif" && /^\d{1,16}$/.test(entry.get("id") || "")) {
    location.replace(pageUrl("gif", { id: entry.get("id"), o: entry.get("o") === "black" ? "black" : null }));
    return;
  }
  toast(t("js.account.signed_in", { name: res.user?.username || "" }), { type: "ok" });
  showDashboard({ focus: true });
}

const PERSONAL = ["[data-me-facts]", "[data-me-badges]", "[data-me-ratings]", "[data-me-sanctions]", "[data-devices]",
  "[data-games-list]", "[data-recent-games]", "[data-games-total]", "[data-me-name]", "[data-me-since]", "[data-me-initial]",
  "[data-email-note]", "[data-delete-label]", "[data-mfa-state]"];

function showAuth(message, { focus = false } = {}) {
  const leaving = view === "dashboard";
  view = "auth";
  me = null;
  dashboardReady = {};
  gamesList = null;
  // Nothing of the previous account stays in the page.
  for (const selector of PERSONAL) {
    const el = $(selector);
    if (el) clear(el);
  }
  for (const form of $$('[data-view="dashboard"] form')) form.reset();
  showView(root(), "auth");
  loadInfo().catch(() => null);
  const name = location.hash.slice(1);
  showAuthPanel(["register", "forgot", "resend"].includes(name) ? name : "signin", { focus: focus || leaving });
  if (message) showMessage($('[data-form="signin"] [data-error]'), message);
}

/* ============================================================================ Google sign-in */

// A plain navigation to Google and back, with nothing loaded from Google: this page makes a PKCE
// pair, the server's start answer gives Google's address, and Google sends the browser back to
// /account/google/ (assets/js/sso-return.js), which hands its answer to this page. The attempt and
// the answer are kept in this tab's sessionStorage, and each is used once.
const GOOGLE_ATTEMPT = "scacelith.sso.attempt";
const GOOGLE_RETURN = "scacelith.sso.return";
let ssoTicket = null;
let linkTicket = null;
let googleRemember = false;

const base64url = (bytes) => btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");

/** Whether the address from the server goes to Google, and brings the browser back to this site. */
function googleUrlOk(authUrl, state) {
  try {
    const url = new URL(authUrl);
    const redirect = url.searchParams.getAll("redirect_uri");
    return url.origin === "https://accounts.google.com" && redirect.length === 1
      && new URL(redirect[0]).origin === location.origin && url.searchParams.get("state") === state;
  } catch {
    return false;
  }
}

async function startGoogle(button) {
  if (isBusy(button)) return;
  const form = button.closest("form");
  resetMessages(form);
  setBusy(button, true);
  try {
    const verifier = base64url(crypto.getRandomValues(new Uint8Array(32)));
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier));
    const res = await api("/auth/sso/google/web/start", { method: "POST", body: { codeChallenge: base64url(new Uint8Array(digest)) } });
    if (!googleUrlOk(res?.authUrl, res?.state)) {
      fail(form, t("js.errors.sso_url"));
    } else if (!store.set(GOOGLE_ATTEMPT, {
      attemptId: res.attemptId, verifier, state: res.state, lang,
      remember: $('[data-form="signin"]').elements.remember.checked,
      search: location.search, createdAt: Date.now(), expiresIn: res.expiresIn,
    }, "session")) {
      fail(form, t("js.errors.sso_storage"));
    } else {
      store.remove(GOOGLE_RETURN, "session");
      location.assign(res.authUrl);
      return; // the button stays busy while the browser leaves
    }
  } catch (e) {
    fail(form, errorMessage(e));
  }
  setBusy(button, false);
}

/** Google's answer, kept by /account/google/, and this tab's attempt: both are taken once. */
function takeGoogleReturn() {
  const answer = store.get(GOOGLE_RETURN, "session");
  if (!answer) return null;
  const attempt = store.get(GOOGLE_ATTEMPT, "session");
  store.remove(GOOGLE_RETURN, "session");
  store.remove(GOOGLE_ATTEMPT, "session");
  return { answer, attempt };
}

async function finishGoogle({ answer, attempt }) {
  const signin = $('[data-form="signin"]');
  const button = $("[data-google-start]", signin);
  // Back from Google: its button shows the work even before the server's information arrives.
  $("[data-google]", signin).hidden = false;
  if (answer.error) return fail(signin, t(answer.error === "access_denied" ? "js.errors.sso_cancelled" : "js.errors.sso_failed"));
  // Nothing goes to the server for an answer that this tab's attempt did not ask for.
  if (!attempt || typeof answer.state !== "string" || answer.state !== attempt.state || typeof answer.code !== "string") {
    return fail(signin, t("js.errors.sso_mismatch"));
  }
  if (!(Date.now() - attempt.createdAt < (attempt.expiresIn || 600) * 1000)) return fail(signin, t("js.errors.sso_expired"));
  googleRemember = Boolean(attempt.remember);
  signin.elements.remember.checked = googleRemember;
  setBusy(button, true);
  signingIn = true;
  try {
    const body = { attemptId: attempt.attemptId, codeVerifier: attempt.verifier, state: answer.state, code: answer.code, clientLabel: clientLabel() };
    if (answer.iss) body.iss = answer.iss;
    const res = await api("/auth/sso/google/finish", { method: "POST", body });
    if (res?.needsUsername) {
      ssoTicket = res.ssoTicket;
      const form = showSigninStep("sso-username");
      resetMessages(form);
      form.reset();
      form.elements.username.value = res.suggestedUsername || "";
      form.elements.username.focus();
    } else if (res?.needsPassword) {
      linkTicket = res.linkTicket;
      const form = showSigninStep("sso-link");
      resetMessages(form);
      form.reset();
      $("[data-link-text]", form).textContent = t("js.account.google_link_text", { name: res.username });
      form.elements.username.value = res.username;
      form.elements.password.focus();
    } else if (res?.mfaRequired) {
      askMfa(res.mfaToken, googleRemember);
    } else if (res?.token) {
      completeSignIn(res, googleRemember);
    } else {
      fail(signin, t("js.errors.sso_failed"));
    }
  } catch (e) {
    fail(signin, errorMessage(e));
  } finally {
    signingIn = false;
    setBusy(button, false);
  }
}

function initGoogle() {
  for (const button of $$("[data-google-start]")) button.addEventListener("click", () => startGoogle(button));
  // Back from Google's page with the Back button, this page may come back from the cache as it left.
  window.addEventListener("pageshow", (event) => {
    if (event.persisted) for (const button of $$("[data-google-start]")) setBusy(button, false);
  });
  const signin = $('[data-form="signin"]');
  // A step that cannot go on: the sign-in form again, with the reason.
  const restart = (e) => {
    showAuthPanel("signin");
    fail(signin, errorMessage(e));
  };

  /* a first Google sign-in: the user name of the new account */
  const named = $('[data-form="sso-username"]');
  named.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(named).submit)) return;
    resetMessages(named);
    const l = limits();
    const username = value(named, "username");
    if (!usernameOk(username, l)) {
      return fail(named, t("js.errors.username_format", { min: l.usernameMin, max: l.usernameMax }), named.elements.username);
    }
    const { submit } = formParts(named);
    setBusy(submit, true);
    signingIn = true;
    try {
      const res = await api("/auth/sso/complete", { method: "POST", body: { ssoTicket, username, clientLabel: clientLabel() } });
      ssoTicket = null;
      named.reset();
      completeSignIn(res, googleRemember);
    } catch (e) {
      // Another name can be tried with the same ticket; the other refusals end this sign-in.
      if (e instanceof ApiError && (e.code === "invalid_username" || e.code === "username_taken")) {
        fail(named, errorMessage(e), named.elements.username);
      } else if (e instanceof ApiError && ["sso_expired", "sso_already_linked", "email_taken", "registration_closed"].includes(e.code)) {
        restart(e);
      } else {
        fail(named, errorMessage(e));
      }
    } finally {
      signingIn = false;
      setBusy(submit, false);
    }
  });

  /* an account with a password uses the Google address: its password links them, once */
  const link = $('[data-form="sso-link"]');
  link.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(link).submit)) return;
    resetMessages(link);
    const password = raw(link, "password");
    if (!password) return fail(link, t("js.errors.required"), link.elements.password);
    const { submit } = formParts(link);
    setBusy(submit, true);
    signingIn = true;
    try {
      const body = { linkTicket, password, clientLabel: clientLabel() };
      const res = await withPow((pow) => api("/auth/sso/google/link", { method: "POST", body: pow ? { ...body, pow } : body }));
      linkTicket = null;
      link.reset();
      // With two-factor authentication on, the link is stored once the code passes.
      if (res.mfaRequired) askMfa(res.mfaToken, googleRemember);
      else completeSignIn(res, googleRemember);
    } catch (e) {
      if (e instanceof ApiError && e.code === "invalid_credentials") {
        fail(link, t("js.errors.invalid_password"), link.elements.password);
      } else if (e instanceof ApiError && (e.code === "sso_expired" || e.code === "sso_already_linked")) {
        link.reset();
        restart(e);
      } else {
        fail(link, errorMessage(e));
      }
    } finally {
      signingIn = false;
      setBusy(submit, false);
    }
  });
}

/* ============================================================================ signed in */

let selectDashTab = null;
let dashboardReady = {};
let gamesList = null;

function badge(text, kind, iconName) {
  return h("li", null, h("span", { class: `badge ${kind || ""}` }, iconName ? icon(iconName) : null, text));
}

function renderMe() {
  const u = me.user;
  updateUser(u);
  $("[data-me-name]").textContent = u.username;
  $("[data-me-initial]").textContent = u.username.slice(0, 1);
  $("[data-me-since]").textContent = t("js.account.member_since", { date: fmt.date(u.createdAt) });
  $("[data-me-public]").href = pageUrl("players", { u: u.username });
  const badges = clear($("[data-me-badges]"));
  if (u.emailVerified) badges.append(badge(t("js.account.badge_verified"), "badge-ok", "check"));
  if (u.mfaEnabled) badges.append(badge(t("js.account.badge_mfa"), "badge-ok", "shield"));
  if (u.googleLinked) badges.append(badge(t("js.account.badge_google"), "", null));

  const ban = $("[data-me-ban]");
  if (me.ban) {
    ban.hidden = false;
    clear(ban).append(icon("warning"), h("span", null, me.ban.until ? t("js.account.ban_until", { date: fmt.dateTime(me.ban.until) }) : t("js.account.ban_permanent")));
  } else {
    ban.hidden = true;
  }

  const facts = clear($("[data-me-facts]"));
  const fact = (label, ...value) => facts.append(h("div", null, h("dt", null, label), h("dd", null, ...value)));
  fact(t("js.account.username"), h("bdi", null, u.username));
  fact(t("js.account.email"), h("bdi", null, u.email || t("js.common.dash")), " ",
    h("span", { class: `badge ${u.emailVerified ? "badge-ok" : "badge-danger"}` }, t(u.emailVerified ? "js.account.verified" : "js.account.unverified")));
  if (u.pendingEmail) fact("", h("span", { class: "hint" }, t("js.account.pending_email", { email: u.pendingEmail })));
  fact(t("js.account.created"), fmt.date(u.createdAt));
  fact(t("js.account.last_login"), u.lastLoginAt ? fmt.dateTime(u.lastLoginAt) : t("js.common.dash"));
  fact(t("js.account.mfa"), t(u.mfaEnabled ? "js.account.mfa_on" : "js.account.mfa_off"));
  fact(t("js.account.google"), t(u.googleLinked ? "js.account.google_on" : "js.account.google_off"));
  fact(t("js.account.password"), t(u.hasPassword ? "js.account.password_set" : "js.account.password_unset"));
  fact(t("js.account.challenges"), t(u.acceptChallenges === "none" ? "js.account.challenges_none" : "js.account.challenges_all"));

  renderRatings($("[data-me-ratings]"), me.ratings, { order: (info?.categories || []).map((c) => c.id) });

  const sanctions = me.sanctions || [];
  $("[data-me-sanctions-panel]").hidden = !sanctions.length;
  const list = clear($("[data-me-sanctions]"));
  for (const s of sanctions) {
    list.append(h("li", null,
      h("strong", null, t(`js.account.sanction_${s.kind}`) || s.kind), " · ",
      s.endsAt ? t("js.account.sanction_until", { date: fmt.dateTime(s.endsAt) }) : t("js.account.sanction_permanent"),
      s.reason ? h("span", { class: "hint" }, ` — ${s.reason}`) : null));
  }

  /* settings that depend on the account */
  const prefs = $("[data-pref-challenges]");
  prefs.checked = u.acceptChallenges !== "none";
  $("[data-mfa-state]").textContent = t(u.mfaEnabled ? "js.account.mfa_state_on" : "js.account.mfa_state_off");
  for (const field of $$("[data-needs-mfa]")) field.hidden = !u.mfaEnabled;
  $("[data-password-note]").textContent = u.hasPassword ? t("js.account.password_note") : t("js.errors.password_not_set");
  $('[data-form="password"]').hidden = false;
  $("[data-email-note]").textContent = u.pendingEmail
    ? t("js.account.email_pending", { email: u.pendingEmail })
    : t("js.account.email_current", { email: u.email || t("js.common.dash") });
  $("[data-delete-label]").textContent = t("js.account.delete_confirm_label", { name: u.username });
}

async function loadMe() {
  me = await api("/account/me", { auth: "required" });
  renderMe();
}

function initGamesTab() {
  const filters = $("[data-games-filters]");
  const select = $("[data-filter-category]");
  for (const c of info?.categories || []) {
    select.insertBefore(h("option", { value: c.id }, ltr(c.id)), select.querySelector('option[value="custom"]'));
  }
  const total = $("[data-games-total]");
  const query = () => {
    const f = new FormData(filters);
    return { category: f.get("category"), rated: f.get("rated"), result: f.get("result") };
  };
  const filtered = () => Object.values(query()).some(Boolean);
  if (!filters.dataset.ready) {
    filters.dataset.ready = "1";
    filters.addEventListener("change", () => {
      total.textContent = "";
      gamesList?.reload();
    });
  }
  gamesList = gameList($("[data-games-list]"), (before) => api("/account/games", { auth: "required", query: { ...query(), before, limit: 20 } }), {
    me: me.user.username,
    owner: me.user.username,
    linkPlayers: true,
    get empty() {
      return t(filtered() ? "js.game.none_filtered" : "js.game.none");
    },
    onTotal: (n) => {
      total.textContent = t("js.game.total", { count: n });
    },
  });
}

async function renderDevices() {
  const list = $("[data-devices]");
  clear(list).append(h("li", { class: "loading-block" }, h("span", { class: "spinner", "aria-hidden": "true" }), t("js.common.loading")));
  try {
    const { sessions } = await api("/auth/sessions", { auth: "required" });
    clear(list);
    for (const s of sessions) {
      const revoke = h("button", { class: "btn btn-ghost btn-small", type: "button" }, icon("logout"), h("span", null, t("js.account.devices_revoke")));
      revoke.addEventListener("click", async () => {
        if (isBusy(revoke)) return;
        setBusy(revoke, true);
        try {
          await api(`/auth/sessions/${encodeURIComponent(s.id)}`, { method: "DELETE", auth: "required" });
          if (s.current) {
            signOutHere();
            return;
          }
          toast(t("js.account.devices_revoked"), { type: "ok" });
          renderDevices();
        } catch (e) {
          toast(errorMessage(e), { type: "error" });
          setBusy(revoke, false);
        }
      });
      list.append(h("li", { class: `device${s.current ? " is-current" : ""}` },
        icon("device"),
        h("div", null,
          h("p", { class: "device-name" }, h("bdi", null, s.clientLabel || t("js.account.devices_unknown")),
            s.current ? h("span", { class: "badge" }, t("js.account.devices_current")) : null),
          h("p", { class: "device-meta" }, [
            t("js.account.devices_signed_in", { date: fmt.date(s.createdAt) }),
            t("js.account.devices_seen", { time: fmt.relative(s.lastSeenAt) }),
            t("js.account.devices_expires", { date: fmt.date(s.expiresAt) }),
          ].join(" · "))),
        revoke));
    }
  } catch (e) {
    clear(list).append(h("li", { class: "form-error" }, errorMessage(e)));
  }
}

function initSettings() {
  const prefs = $("[data-pref-challenges]");
  let saving = false;
  prefs.addEventListener("change", async () => {
    if (saving) {
      prefs.checked = !prefs.checked;
      return;
    }
    saving = true;
    prefs.setAttribute("aria-busy", "true");
    const acceptChallenges = prefs.checked ? "all" : "none";
    try {
      await api("/account/preferences", { method: "PUT", auth: "required", body: { acceptChallenges } });
      me.user.acceptChallenges = acceptChallenges;
      renderMe();
      toast(t("js.account.prefs_saved"), { type: "ok", timeout: 2000 });
    } catch (e) {
      prefs.checked = !prefs.checked;
      toast(errorMessage(e), { type: "error" });
    } finally {
      saving = false;
      prefs.removeAttribute("aria-busy");
    }
  });

  const reauth = (form) => {
    const body = { password: raw(form, "password") };
    const code = value(form, "code");
    if (me?.user?.mfaEnabled && code) body.code = code.replace(/\s+/g, "");
    return body;
  };
  const needMfa = (form) => me?.user?.mfaEnabled && !value(form, "code");
  // Two-step verification turned on elsewhere (in the game) since this page read the account:
  // read it again, which shows the code fields, before pointing at one.
  const showMfaFields = async (e) => {
    if (e?.code === "mfa_code_required" && !me?.user?.mfaEnabled) await loadMe().catch(() => null);
  };

  const password = $('[data-form="password"]');
  password.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(password).submit)) return;
    resetMessages(password);
    const l = limits();
    const current = raw(password, "currentPassword");
    const next = raw(password, "newPassword");
    if (!current) return fail(password, t("js.errors.required"), password.elements.currentPassword);
    if ([...next].length < l.passwordMinLength) return fail(password, t("js.errors.password_length", { min: l.passwordMinLength }), password.elements.newPassword);
    if (next !== raw(password, "confirm")) return fail(password, t("js.errors.passwords_differ"), password.elements.confirm);
    const { submit, success } = formParts(password);
    setBusy(submit, true);
    try {
      await api("/account/password", { method: "POST", auth: "required", body: { currentPassword: current, newPassword: next } });
      password.reset();
      showMessage(success, t("js.account.password_changed"));
    } catch (e) {
      fail(password, errorMessage(e, { passwordMin: l.passwordMinLength }), e.code === "invalid_password" ? password.elements.currentPassword : e.code === "weak_password" ? password.elements.newPassword : null);
    } finally {
      setBusy(submit, false);
    }
  });

  const email = $('[data-form="email"]');
  email.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(email).submit)) return;
    resetMessages(email);
    const newEmail = value(email, "newEmail");
    if (!looksLikeEmail(newEmail)) return fail(email, t("js.errors.email_format"), email.elements.newEmail);
    if (!raw(email, "password")) return fail(email, t("js.errors.required"), email.elements.password);
    if (needMfa(email)) return fail(email, t("js.errors.mfa_code_required"), email.elements.code);
    const { submit, success } = formParts(email);
    setBusy(submit, true);
    try {
      const res = await api("/account/email", { method: "POST", auth: "required", body: { newEmail, ...reauth(email) } });
      email.reset();
      showMessage(success, res?.status === "email_changed" ? t("js.account.email_changed", { email: res.email }) : t("js.account.email_sent"));
      loadMe().catch(() => null);
    } catch (e) {
      const field = { invalid_email: "newEmail", same_email: "newEmail", email_taken: "newEmail", invalid_password: "password", invalid_code: "code", mfa_code_required: "code" }[e.code];
      await showMfaFields(e);
      fail(email, errorMessage(e), field ? email.elements[field] : null);
    } finally {
      setBusy(submit, false);
    }
  });

  const exportForm = $('[data-form="export"]');
  exportForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(exportForm).submit)) return;
    resetMessages(exportForm);
    if (!raw(exportForm, "password")) return fail(exportForm, t("js.errors.required"), exportForm.elements.password);
    if (needMfa(exportForm)) return fail(exportForm, t("js.errors.mfa_code_required"), exportForm.elements.code);
    const { submit, success } = formParts(exportForm);
    setBusy(submit, true);
    try {
      const res = await api("/account/export", { method: "POST", auth: "required", body: reauth(exportForm), as: "response", timeout: 70000 });
      const blob = await res.blob();
      saveBlob(blob, fileNameOf(res, `scacelith-account-${me.user.username.replace(/[^\w.-]/g, "_")}.json`));
      exportForm.reset();
      showMessage(success, t("js.account.export_done"));
    } catch (e) {
      const field = { invalid_password: "password", invalid_code: "code", mfa_code_required: "code" }[e.code];
      await showMfaFields(e);
      fail(exportForm, errorMessage(e), field ? exportForm.elements[field] : null);
    } finally {
      setBusy(submit, false);
    }
  });

  const del = $('[data-form="delete"]');
  del.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (isBusy(formParts(del).submit)) return;
    resetMessages(del);
    if (value(del, "confirmName") !== me.user.username) return fail(del, t("js.errors.confirm_name"), del.elements.confirmName);
    if (!raw(del, "password")) return fail(del, t("js.errors.required"), del.elements.password);
    if (needMfa(del)) return fail(del, t("js.errors.mfa_code_required"), del.elements.code);
    const { submit } = formParts(del);
    setBusy(submit, true);
    try {
      await api("/account/delete", { method: "POST", auth: "required", body: reauth(del) });
      del.reset();
      signOutHere();
      toast(t("js.account.deleted"), { type: "ok", timeout: 7000 });
    } catch (e) {
      const field = { invalid_password: "password", invalid_code: "code", mfa_code_required: "code" }[e.code];
      await showMfaFields(e);
      fail(del, errorMessage(e), field ? del.elements[field] : null);
    } finally {
      setBusy(submit, false);
    }
  });

  $("[data-logout]").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    if (isBusy(button)) return;
    setBusy(button, true);
    try {
      await api("/auth/logout", { method: "POST", auth: "required" });
    } catch {
      /* signed out here either way */
    } finally {
      setBusy(button, false);
      signOutHere();
      toast(t("js.account.signed_out"), { type: "ok" });
    }
  });
  $("[data-logout-all]").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    if (isBusy(button) || !window.confirm(t("js.account.signout_all_confirm"))) return;
    setBusy(button, true);
    try {
      await api("/auth/logout-all", { method: "POST", auth: "required" });
      signOutHere();
      toast(t("js.account.signed_out"), { type: "ok" });
    } catch (e) {
      toast(errorMessage(e), { type: "error" });
    } finally {
      setBusy(button, false);
    }
  });
}

function onDashTab(name) {
  const hash = name === "overview" ? "" : `#${name}`;
  if (location.hash !== hash) history.replaceState(null, "", hash || location.pathname + location.search);
  if (!me || dashboardReady[name]) return;
  dashboardReady[name] = true;
  if (name === "games") initGamesTab();
  if (name === "devices") renderDevices();
  if (name === "overview") {
    gameList($("[data-recent-games]"), () => api("/account/games", { auth: "required", query: { limit: 5 } }), {
      me: me.user.username,
      owner: me.user.username,
      linkPlayers: true,
      limit: 5,
      empty: t("js.game.none"),
    });
  }
}

async function showDashboard({ focus = false } = {}) {
  view = "dashboard";
  showView(root(), "loading");
  dashboardReady = {};
  gamesList = null;
  $("[data-games-total]").textContent = "";
  try {
    await Promise.all([loadInfo().catch(() => null), loadMe()]);
  } catch (e) {
    if (!getSession()) {
      showAuth(errorMessage(e));
      return;
    }
    showView(root(), "loading");
    const box = $('[data-view="loading"]');
    const retry = h("button", { class: "btn btn-ghost btn-small", type: "button" }, icon("refresh"), h("span", null, t("js.common.retry")));
    retry.addEventListener("click", () => showDashboard());
    clear(box).append(h("p", { class: "form-error", role: "alert" }, errorMessage(e)), retry);
    return;
  }
  // Reset the lists of a previous account.
  clear($("[data-games-list]"));
  clear($("[data-recent-games]"));
  clear($("[data-devices]"));
  const select = $("[data-filter-category]");
  for (const option of $$("option", select)) if (option.value && option.value !== "custom") option.remove();
  $("[data-games-filters]").reset();

  showView(root(), "dashboard");
  const name = pendingTab || location.hash.slice(1);
  pendingTab = null;
  selectDashTab(["games", "devices", "settings"].includes(name) ? name : "overview");
  if (focus) $("[data-me-name]").focus();
}

/* ============================================================================ start */

onReady(() => {
  initAuth();
  selectDashTab = initTabs($("[data-dash-tabs]"), (name) => $(`[data-panel="${name}"]`, $('[data-view="dashboard"]')), onDashTab);
  for (const link of $$("[data-goto-tab]")) {
    link.addEventListener("click", (event) => {
      event.preventDefault();
      selectDashTab(link.dataset.gotoTab, { focus: true });
      $("[data-dash-tabs]").scrollIntoView({ block: "start", behavior: "smooth" });
    });
  }
  initSettings();

  onSessionChange((session) => {
    if (!session && view === "dashboard") {
      showAuth(signingOut ? "" : t("js.errors.invalid_token"), { focus: true });
      signingOut = false;
    }
    else if (session && view === "auth" && !signingIn) showDashboard();
  });
  window.addEventListener("hashchange", () => {
    const name = location.hash.slice(1);
    if (!["", "signin", "register", "forgot", "resend", "overview", "games", "devices", "settings"].includes(name)) return;
    if (view === "auth") showAuthPanel(["register", "forgot", "resend"].includes(name) ? name : "signin");
    else if (view === "dashboard") selectDashTab(["games", "devices", "settings"].includes(name) ? name : "overview");
  });

  // Back from Google (/account/google/): its answer is taken before anything else, used or not.
  const google = takeGoogleReturn();
  if (getSession()) {
    showDashboard();
  } else if (google) {
    showAuth();
    finishGoogle(google);
  } else {
    showAuth();
    requestSession(350).then((s) => {
      if (s && view === "auth" && !signingIn) showDashboard();
    });
  }
});
