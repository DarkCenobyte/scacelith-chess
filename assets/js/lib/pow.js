// Solves a proof-of-work challenge of the server in a worker, off the main thread.

export function solvePow(challenge, bits, onProgress) {
  return new Promise((resolve, reject) => {
    let worker;
    try {
      worker = new Worker(new URL("../pow-worker.js", import.meta.url));
    } catch (e) {
      reject(e);
      return;
    }
    const stop = () => worker.terminate();
    worker.onmessage = (event) => {
      const msg = event.data || {};
      if (msg.type === "progress") onProgress?.(msg.tries, msg.expected);
      else if (msg.type === "done") {
        stop();
        resolve(msg.nonce);
      } else if (msg.type === "error") {
        stop();
        reject(new Error(msg.message));
      }
    };
    worker.onerror = (event) => {
      stop();
      reject(new Error(event.message || "worker error"));
    };
    worker.postMessage({ challenge, bits: Number(bits) });
  });
}
