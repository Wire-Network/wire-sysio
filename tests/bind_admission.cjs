// FIFO scheduling metadata only. BindConfigProvider remains the sole issuer
// and registrar of ports after a native custodian reaches this queue's head.
const fs = require("node:fs");
const path = require("node:path");
const { randomUUID } = require("node:crypto");
const { performance } = require("node:perf_hooks");

const QueueFilename = "native-admission.json";
const QueueLockFilename = "native-admission";
const PollMilliseconds = 50;
const CleanupMilliseconds = 60_000;
const LockOptions = { realpath: false, retries: 0, stale: 30_000 };
const pause = () => new Promise(resolve => setTimeout(resolve, PollMilliseconds));

/**
 * Admit one custodian in enqueue order, until its complete allocation finishes.
 * deadline is a Date.now() deadline supplied by the caller; it is converted
 * once to a monotonic remaining budget. The returned async release is idempotent.
 * Cancellation/timeout removes this ticket; a killed issuer is reaped by the
 * next queue reader using the canonical provider's process-liveness helpers.
 */
async function acquire({ requireSource, deadline, isStopping }) {
  if (!Number.isFinite(deadline) || typeof isStopping !== "function") {
    throw new Error("Native admission requires a wall-clock deadline and cancellation predicate");
  }
  const expires = performance.now() + Math.max(0, deadline - Date.now());
  const { BindConfigProvider } = requireSource("@wireio/cluster-tool/lib/cjs/config/BindConfigProvider.js");
  const { withFileLock } = requireSource("@wireio/cluster-tool/lib/cjs/utils/fsUtils.js");
  const { isPidAlive, processCommandBasename } = requireSource("@wireio/cluster-tool/lib/cjs/utils/processUtils.js");
  const directory = BindConfigProvider.registryPath();
  const filename = path.join(directory, QueueFilename);
  const lock = path.join(directory, QueueLockFilename);
  const ticket = { pid: process.pid, token: randomUUID() };
  let enqueued = false;
  let releasePromise;

  function readLive() {
    let entries;
    try { entries = JSON.parse(fs.readFileSync(filename, "utf8")); }
    catch (error) { if (error.code === "ENOENT") return []; throw error; }
    if (!Array.isArray(entries) || entries.some(entry =>
      !Number.isSafeInteger(entry.pid) || entry.pid <= 0 ||
      typeof entry.token !== "string" || !/^[0-9a-f-]{36}$/.test(entry.token))) {
      throw new Error("Malformed native admission queue");
    }
    return entries.filter(entry => {
      if (!isPidAlive(entry.pid)) return false;
      const basename = processCommandBasename(entry.pid);
      // As in the registry, unreadable process metadata is kept conservatively.
      return basename === "" || basename === "node";
    });
  }

  function write(entries) {
    const temporary = `${filename}.${process.pid}.${randomUUID()}.tmp`;
    try {
      fs.writeFileSync(temporary, JSON.stringify(entries), { flag: "wx" });
      fs.renameSync(temporary, filename);
    } finally {
      fs.rmSync(temporary, { force: true });
    }
  }

  async function update(change, until, cancelled = () => false) {
    for (;;) {
      if (cancelled()) throw Object.assign(new Error("Native admission cancelled"), { code: "ADMISSION_CANCELLED" });
      if (performance.now() >= until) throw Object.assign(new Error("Native admission deadline exceeded"), { code: "ADMISSION_TIMEOUT" });
      try {
        return await withFileLock(lock, () => change(readLive()), LockOptions);
      } catch (error) {
        if (error.code !== "ELOCKED") throw error;
        await pause();
      }
    }
  }

  function release() {
    if (!releasePromise) {
      releasePromise = enqueued ? update(entries => {
        write(entries.filter(entry => entry.token !== ticket.token));
      }, performance.now() + CleanupMilliseconds) : Promise.resolve();
    }
    return releasePromise;
  }

  try {
    await update(entries => {
      entries.push(ticket);
      write(entries);
      enqueued = true;
    }, expires, isStopping);
    for (;;) {
      const first = await update(entries => {
        write(entries);
        return entries[0]?.token;
      }, expires, isStopping);
      if (first === ticket.token) return release;
      await pause();
    }
  } catch (error) {
    try { await release(); }
    catch (cleanupError) { throw new AggregateError([error, cleanupError], "Native admission and ticket cleanup failed"); }
    throw error;
  }
}

module.exports = { acquire };
