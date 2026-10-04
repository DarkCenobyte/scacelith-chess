// Proof of work of the Scacelith server (API reference, section 1.6): find a decimal nonce such
// that SHA-256(challenge + ":" + nonce) starts with `bits` zero bits. 18 bits take about 260,000
// hashes on average. The blocks of the fixed prefix are hashed once; each try only hashes the
// last one or two blocks.

"use strict";

const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);
const H0 = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

const W = new Uint32Array(64);

function compress(state, bytes, offset) {
  for (let i = 0; i < 16; i++) {
    const j = offset + i * 4;
    W[i] = (bytes[j] << 24) | (bytes[j + 1] << 16) | (bytes[j + 2] << 8) | bytes[j + 3];
  }
  for (let i = 16; i < 64; i++) {
    const w15 = W[i - 15], w2 = W[i - 2];
    const s0 = ((w15 >>> 7) | (w15 << 25)) ^ ((w15 >>> 18) | (w15 << 14)) ^ (w15 >>> 3);
    const s1 = ((w2 >>> 17) | (w2 << 15)) ^ ((w2 >>> 19) | (w2 << 13)) ^ (w2 >>> 10);
    W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
  }
  let a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
  for (let i = 0; i < 64; i++) {
    const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
    const ch = (e & f) ^ (~e & g);
    const t1 = (h + S1 + ch + K[i] + W[i]) | 0;
    const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
    const maj = (a & b) ^ (a & c) ^ (b & c);
    const t2 = (S0 + maj) | 0;
    h = g; g = f; f = e; e = (d + t1) | 0;
    d = c; c = b; b = a; a = (t1 + t2) | 0;
  }
  state[0] = (state[0] + a) | 0; state[1] = (state[1] + b) | 0;
  state[2] = (state[2] + c) | 0; state[3] = (state[3] + d) | 0;
  state[4] = (state[4] + e) | 0; state[5] = (state[5] + f) | 0;
  state[6] = (state[6] + g) | 0; state[7] = (state[7] + h) | 0;
}

function hasZeroBits(state, bits) {
  let i = 0;
  for (; bits >= 32; bits -= 32, i++) if (state[i] !== 0) return false;
  return bits === 0 || state[i] >>> (32 - bits) === 0;
}

function solve(challenge, bits) {
  const prefix = new TextEncoder().encode(challenge + ":");
  const full = Math.floor(prefix.length / 64);
  const mid = new Uint32Array(H0);
  for (let b = 0; b < full; b++) compress(mid, prefix, b * 64);
  const tail = prefix.subarray(full * 64);
  const buf = new Uint8Array(128);
  buf.set(tail);
  const state = new Uint32Array(8);
  const started = Date.now();
  let lastReport = started;

  for (let nonce = 0; nonce < Number.MAX_SAFE_INTEGER; nonce++) {
    const digits = String(nonce);
    let p = tail.length;
    for (let i = 0; i < digits.length; i++) buf[p++] = digits.charCodeAt(i);
    const length = prefix.length + digits.length;
    buf[p++] = 0x80;
    const end = p + 8 <= 64 ? 64 : 128;
    buf.fill(0, p, end - 8);
    const bitsHi = Math.floor((length * 8) / 0x100000000);
    const bitsLo = (length * 8) >>> 0;
    buf[end - 8] = bitsHi >>> 24; buf[end - 7] = bitsHi >>> 16; buf[end - 6] = bitsHi >>> 8; buf[end - 5] = bitsHi;
    buf[end - 4] = bitsLo >>> 24; buf[end - 3] = bitsLo >>> 16; buf[end - 2] = bitsLo >>> 8; buf[end - 1] = bitsLo;
    state.set(mid);
    compress(state, buf, 0);
    if (end === 128) compress(state, buf, 64);
    if (hasZeroBits(state, bits)) return { nonce: digits, tries: nonce + 1, ms: Date.now() - started };
    if ((nonce & 0x3fff) === 0) {
      const now = Date.now();
      if (now - lastReport > 250) {
        lastReport = now;
        self.postMessage({ type: "progress", tries: nonce + 1, expected: 2 ** bits });
      }
    }
  }
  throw new Error("no nonce found");
}

self.onmessage = (event) => {
  const { challenge, bits } = event.data || {};
  try {
    if (typeof challenge !== "string" || !Number.isInteger(bits) || bits < 0 || bits > 64) throw new Error("bad challenge");
    self.postMessage({ type: "done", ...solve(challenge, bits) });
  } catch (e) {
    self.postMessage({ type: "error", message: String(e && e.message ? e.message : e) });
  }
};
