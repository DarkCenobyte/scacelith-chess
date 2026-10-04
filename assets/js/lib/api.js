// The HTTPS API of the official server (docs.scacelith.com). Every answer is JSON except the PGN,
// the GIF and the data export; errors have one shape: { error, message, retryAfter?, field?, ... }.

import { getSession, clearSession } from "./session.js";
import { solvePow } from "./pow.js";

export const API = (document.documentElement.dataset.api || "").replace(/\/+$/, "");

export class ApiError extends Error {
  constructor(status, body = {}, headers = null) {
    super(body.message || body.error || `HTTP ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.code = body.error || (status ? `http_${status}` : "network");
    this.body = body;
    const header = headers?.get?.("Retry-After");
    this.retryAfter = Number(body.retryAfter ?? (header !== null && header !== undefined ? Number(header) : NaN));
    if (!Number.isFinite(this.retryAfter)) this.retryAfter = null;
    this.field = body.field ?? null;
    this.reason = body.reason ?? null;
  }
}

function buildUrl(path, query) {
  let url = API + path;
  if (query) {
    const params = new URLSearchParams();
    for (const [k, v] of Object.entries(query)) if (v !== undefined && v !== null && v !== "") params.set(k, String(v));
    const qs = params.toString();
    if (qs) url += `?${qs}`;
  }
  return url;
}

/**
 * api("/account/me", { auth: "required" })
 * auth: "none" (default), "optional" (sends the token when signed in) or "required".
 * as: "json" (default), "text", "blob" or "response".
 */
export async function api(path, { method = "GET", body, query, auth = "none", as = "json", timeout = 30000, signal } = {}) {
  const headers = { Accept: as === "json" ? "application/json" : "*/*" };
  const session = auth === "none" ? null : getSession();
  if (auth === "required" && !session) {
    // The session expired while the page was open: let the page show the sign-in again.
    clearSession({ broadcast: false });
    throw new ApiError(401, { error: "invalid_token" });
  }
  if (session) headers.Authorization = `Bearer ${session.token}`;
  let payload;
  if (body !== undefined) {
    headers["Content-Type"] = "application/json";
    payload = JSON.stringify(body);
  }

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(new DOMException("timeout", "TimeoutError")), timeout);
  const onAbort = () => controller.abort(signal.reason);
  signal?.addEventListener("abort", onAbort, { once: true });

  let res;
  try {
    res = await fetch(buildUrl(path, query), {
      method,
      headers,
      body: payload,
      mode: "cors",
      credentials: "omit",
      cache: "no-store",
      referrerPolicy: "no-referrer",
      signal: controller.signal,
    });
  } catch (e) {
    if (signal?.aborted) throw e;
    throw new ApiError(0, { error: controller.signal.aborted ? "timeout" : "network" });
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener("abort", onAbort);
  }

  if (!res.ok) {
    let errorBody = {};
    try {
      if ((res.headers.get("Content-Type") || "").includes("json")) errorBody = await res.json();
    } catch {
      /* not JSON */
    }
    const error = new ApiError(res.status, errorBody, res.headers);
    if (session && (error.code === "invalid_token" || (res.status === 401 && auth === "required"))) {
      clearSession();
      // A session revoked or expired on the server: a public answer does not need it.
      if (auth === "optional") return api(path, { method, body, query, auth: "none", as, timeout, signal });
    }
    throw error;
  }
  if (as === "response") return res;
  if (as === "text") return res.text();
  if (as === "blob") return res.blob();
  if (res.status === 204) return null;
  const text = await res.text();
  return text ? JSON.parse(text) : null;
}

/**
 * Sends a request that may need a proof of work (docs: section 1.6): `send(pow)` is called without
 * one first; on 428 pow_required the challenge is solved in a worker and the request sent again.
 */
export async function withPow(send, onSolving) {
  let pow;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      return await send(pow);
    } catch (e) {
      if (!(e instanceof ApiError) || e.code !== "pow_required" || !e.body.pow?.challenge) throw e;
      onSolving?.(true);
      try {
        const nonce = await solvePow(e.body.pow.challenge, e.body.pow.bits);
        pow = { challenge: e.body.pow.challenge, nonce };
      } finally {
        onSolving?.(false);
      }
    }
  }
  throw new ApiError(428, { error: "pow_failed" });
}

/** A file name from Content-Disposition, or the fallback. */
export function fileNameOf(res, fallback) {
  const cd = res.headers.get("Content-Disposition") || "";
  const m = /filename\*=UTF-8''([^;]+)|filename="?([^";]+)"?/i.exec(cd);
  try {
    const name = m ? decodeURIComponent(m[1] || m[2]) : "";
    return /^[\w.-]{1,128}$/.test(name) ? name : fallback;
  } catch {
    return fallback;
  }
}
