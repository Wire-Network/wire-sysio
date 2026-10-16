// The Node process both issues the canonical lease and owns the test process
// group. No daemon can be spawned before its custodian knows that group.
const { createRequire } = require("node:module");
const path = require("node:path");
const fs = require("node:fs");
const { execFileSync, spawn } = require("node:child_process");
const root = process.argv[2];
if (!root || !path.isAbsolute(root)) throw new Error("An absolute wire-sysio source directory is required");
const requireSource = createRequire(path.join(root, "package.json"));
const { BindConfigProvider } = requireSource("@wireio/cluster-tool/lib/cjs/config/BindConfigProvider.js");
let child = null;
let stopping = false;
let admissionPending;
let releaseAdmission;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function groupAlive() {
  return child !== null && execFileSync("ps", ["-eo", "pgid=,stat="], {encoding: "utf8"})
    .trim().split("\n").some(line => {
      const [group, state] = line.trim().split(/\s+/);
      return Number(group) === child.pid && !state.startsWith("Z");
    });
}
function signalGroup(signal) {
  try { process.kill(-child.pid, signal); }
  catch (error) { if (error.code !== "ESRCH") throw error; }
}
async function stop(status) {
  if (stopping) return;
  stopping = true;
  if (groupAlive()) {
    signalGroup("SIGTERM");
    const deadline = Date.now() + 30_000;
    while (groupAlive() && Date.now() < deadline) await delay(100);
    if (groupAlive()) signalGroup("SIGKILL");
    // A zombie cannot hold sockets. Never release a live descendant's lease.
    while (groupAlive()) await delay(100);
  }
  if (admissionPending) {
    try { await admissionPending; } catch (_) { /* acquire removes cancelled tickets */ }
  }
  if (releaseAdmission) await releaseAdmission();
  process.exit(status);
}
function stopSafely(status) {
  stop(status).catch(error => {
    console.error(error);
    // Keep the registration alive if cleanup cannot establish safety.
    setInterval(() => {}, 60_000);
  });
}
// Observe ownership before potentially contended allocation. A killed wrapper
// closes this pipe, including while no test process has been started yet.
process.stdin.resume();
process.stdin.on("end", () => stopSafely(0));
process.on("SIGTERM", () => stopSafely(143));
process.on("SIGINT", () => stopSafely(130));
async function main() {
  const deadline = Date.now() + 900_000;
  const ports = [];
  const width = BindConfigProvider.SolanaDynamicPortRangeSize;
  const agave = BindConfigProvider.ReservedAgavePortBand;
  const firstPreference = 1024; // unprivileged preferences, still canonically validated
  // Never consume the host's OS-assigned ephemeral pool. Both low and high
  // non-ephemeral bands remain usable, so a highly parallel CTest run does not
  // inherit the canonical resolver's small default validator-window limit.
  let ephemeral;
  if (process.platform === "linux") {
    ephemeral = fs.readFileSync("/proc/sys/net/ipv4/ip_local_port_range", "utf8").trim().split(/\s+/).map(Number);
  } else if (process.platform === "darwin") {
    ephemeral = execFileSync("/usr/sbin/sysctl", ["-n", "net.inet.ip.portrange.first", "net.inet.ip.portrange.last"],
                             {encoding: "utf8"}).trim().split(/\s+/).map(Number);
  } else {
    throw new Error(`Native registry preferences do not support ${process.platform}`);
  }
  if (ephemeral.length !== 2 || !ephemeral.every(Number.isSafeInteger) ||
      ephemeral[0] < 1 || ephemeral[1] > 65535 || ephemeral[0] > ephemeral[1]) {
    throw new Error("Cannot determine the host ephemeral port band");
  }
  const auxiliaryPortCount = 9; // includes the RPC's implicit websocket slot
  const blockWidth = width + auxiliaryPortCount;
  const candidates = [];
  for (let first = firstPreference; first + blockWidth - 1 <= 65535; first += blockWidth) {
    const last = first + blockWidth - 1;
    if ((last < ephemeral[0] || first > ephemeral[1]) &&
        (last < agave.first || first > agave.last)) candidates.push(first);
  }
  const windowCount = candidates.length;
  if (windowCount < 3) throw new Error("Host has insufficient non-ephemeral native reservation capacity");
  const offset = Number(process.env.SYSIO_TEST_PORT_OFFSET ?? 0);
  if (!Number.isSafeInteger(offset) || offset < 0) throw new Error("Invalid native test port preference offset");
  let preference = offset % windowCount;
  const queued = Date.now();
  admissionPending = require("./bind_admission.cjs").acquire({
    requireSource, deadline, isStopping: () => stopping
  });
  releaseAdmission = await admissionPending;
  if (stopping) return;
  console.error(`Native allocation admitted after ${Date.now() - queued}ms in FIFO queue`);
  const started = Date.now();
  let lockContentions = 0;
  try {
    // Reuse the canonical resolver's contiguous 64-port reservation surface.
    // These are generic TCP+UDP-checked reservations: no validator is started.
    // Three full configs are registered under this custodian's Node PID. Pinned
    // preferences extend beyond the resolver's default 32-window search band;
    // the resolver still probes/excludes/registers every candidate atomically.
    // This avoids depending on 192 random draws from a potentially tiny host kernel
    // ephemeral range; no port is selected outside BindConfigProvider.resolve.
    for (let window = 0; window < 3 && !stopping; ++window) {
      let bind;
      let rejected = 0;
      while (!bind && !stopping) {
        const first = candidates[preference];
        const last = first + width - 1;
        // The canonical BindConfig also reserves auxiliary daemon roles even
        // though native tests never launch those daemons. Pin their preferences
        // in the nine adjacent slots too, so none depend on the OS ephemeral pool.
        const aux = first + width;
        const options = {
          kiod: {port: aux},
          nodeop: {ports: {bios: {http: aux + 1, p2p: aux + 2}}},
          anvil: {port: aux + 3},
          solana: {ports: {http: aux + 4, faucet: aux + 6, gossip: aux + 7,
                           dynamicRange: {first, last}}},
          debuggingServer: {port: aux + 8}
        };
        const unavailable = new Set([
          [aux, "kiod"], [aux + 1, "nodeop.bios.http"], [aux + 2, "nodeop.bios.p2p"],
          [aux + 3, "anvil"], [aux + 4, "solana.http"], [aux + 6, "solana.faucet"],
          [aux + 7, "solana.gossip"], [aux + 8, "debuggingServer"]
        ].map(([port, role]) => `port ${port} for ${role} is pinned but unavailable`));
        unavailable.add(`port range ${first}-${last} for solana.dynamicRange is pinned but unavailable`);
        try {
          bind = await BindConfigProvider.resolve(options, {
            producerCount: 0, batchOperatorCount: 0, underwriterCount: 0,
            apiCount: 0, adHocCount: 0
          });
        } catch (error) {
          if (Date.now() >= deadline) throw error;
          if (error.code === "ELOCKED") {
            ++lockContentions;
            await delay(100);
          } else if (error.code === "ERR_ASSERTION" &&
                     unavailable.has(error.message) &&
                     ++rejected < windowCount) {
            preference = (preference + 1) % windowCount;
          } else {
            throw error;
          }
        }
      }
      if (stopping) return;
      const {first, last} = bind.solana.ports.dynamicRange;
      for (let port = first; port <= last; ++port) ports.push(port);
      preference = (preference + 1) % windowCount;
    }
  } finally {
    await releaseAdmission();
  }
  console.error(`Native canonical lease allocated in ${Date.now() - started}ms (${lockContentions} lock-contention retries)`);
  if (stopping) return;
  if (ports.length !== 192 || new Set(ports).size !== 192) throw new Error("Invalid canonical native lease");
  const [command, ...args] = process.argv.slice(3);
  if (!command) throw new Error("A test command is required");
  child = spawn(command, args, {
    detached: true,
    stdio: ["ignore", process.stdout, process.stderr],
    env: {...process.env, SYSIO_TEST_PORTS: ports.join(","), SYSIO_TEST_PORT_ISSUER_PID: String(process.pid)}
  });
  const behaviorTimeout = Number(process.env.SYSIO_TEST_BEHAVIOR_TIMEOUT ?? 0);
  const timer = behaviorTimeout > 0 ? setTimeout(() => {
    console.error(`Native test exceeded its ${behaviorTimeout}-second behavioral deadline`);
    stopSafely(124);
  }, behaviorTimeout * 1000) : null;
  child.on("error", error => { console.error(error); stopSafely(1); });
  child.on("exit", (code, signal) => {
    if (timer !== null) clearTimeout(timer);
    stopSafely(code ?? (signal === "SIGINT" ? 130 : 143));
  });
}
main().catch(error => { console.error(error); stopSafely(1); });
