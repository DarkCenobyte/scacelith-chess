// The signed-in session: the bearer token of the official server and its account view.
//
// "Keep me signed in" stores it in localStorage; otherwise it lives in sessionStorage, for this
// tab only. Tabs of the site talk through a BroadcastChannel: a tab opened from another one (the
// GIF page) asks for the session, and signing out anywhere signs out everywhere.

import * as store from "./store.js";

const KEY = "scacelith.session";
const API = document.documentElement.dataset.api || "";
const channel = typeof BroadcastChannel === "function" ? new BroadcastChannel("scacelith-session") : null;
const listeners = new Set();
let memory = null;

function valid(s) {
  return Boolean(s && typeof s.token === "string" && s.token && s.api === API && (!s.expiresAt || s.expiresAt > Date.now()));
}

function emit() {
  const s = getSession();
  for (const fn of listeners) {
    try {
      fn(s);
    } catch (e) {
      console.error(e);
    }
  }
}

export function getSession() {
  if (valid(memory)) return memory;
  const stored = store.get(KEY, "session") || store.get(KEY, "local");
  if (valid(stored)) {
    memory = stored;
    return memory;
  }
  memory = null;
  return null;
}

export function setSession({ token, expiresAt, user }, remember) {
  memory = { token, expiresAt, user, api: API, remember: Boolean(remember) };
  store.remove(KEY, remember ? "session" : "local");
  store.set(KEY, memory, remember ? "local" : "session");
  channel?.postMessage({ type: "login" });
  emit();
  return memory;
}

export function updateUser(user) {
  const s = getSession();
  if (!s) return;
  memory = { ...s, user: { ...s.user, ...user } };
  store.set(KEY, memory, memory.remember ? "local" : "session");
  emit();
}

export function clearSession({ broadcast = true } = {}) {
  memory = null;
  store.remove(KEY, "session");
  store.remove(KEY, "local");
  if (broadcast) channel?.postMessage({ type: "logout" });
  emit();
}

export function onSessionChange(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

/** The session of this tab, or else one shared by another open tab of the site. */
export function requestSession(timeoutMs = 800) {
  const own = getSession();
  if (own || !channel) return Promise.resolve(own);
  const id = Math.random().toString(36).slice(2);
  return new Promise((resolve) => {
    const done = (s) => {
      channel.removeEventListener("message", onMessage);
      clearTimeout(timer);
      resolve(s);
    };
    const onMessage = (event) => {
      const msg = event.data || {};
      if (msg.type === "share" && msg.id === id && valid(msg.session)) {
        memory = msg.session;
        store.set(KEY, memory, memory.remember ? "local" : "session");
        done(memory);
        emit();
      }
    };
    const timer = setTimeout(() => done(null), timeoutMs);
    channel.addEventListener("message", onMessage);
    channel.postMessage({ type: "request", id });
  });
}

channel?.addEventListener("message", (event) => {
  const msg = event.data || {};
  if (msg.type === "request") {
    const s = getSession();
    if (s) channel.postMessage({ type: "share", id: msg.id, session: s });
  } else if (msg.type === "logout") {
    memory = null;
    store.remove(KEY, "session");
    emit();
  } else if (msg.type === "login") {
    memory = null;
    if (getSession()) emit();
    else requestSession();
  }
});

window.addEventListener("storage", (event) => {
  if (event.key === KEY || event.key === null) {
    memory = null;
    emit();
  }
});
