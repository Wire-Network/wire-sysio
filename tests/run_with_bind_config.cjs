/** Run native network tests while BindConfigProvider holds their port reservations. */
const path = require('node:path');
const { spawn } = require('node:child_process');

/** Keep the provider process alive until the native child exits. */
async function main() {
  const toolsRoot = process.env.WIRE_TOOLS_TS_ROOT;
  const providerModule = toolsRoot
    ? path.join(toolsRoot, 'packages/cluster-tool/lib/cjs/config/BindConfigProvider.js')
    : '@wireio/cluster-tool/lib/cjs/config/BindConfigProvider.js';
  const { BindConfigProvider } = require(providerModule);
  const reservationPairs = 64;
  const bind = await BindConfigProvider.resolve({}, {
    producerCount: 0, batchOperatorCount: 0, underwriterCount: 0, apiCount: 0,
    adHocCount: reservationPairs, bindAll: false
  });
  const ports = bind.nodeop.ports.adHoc.flatMap(pair => [pair.http, pair.p2p]);
  const [command, ...args] = process.argv.slice(2);
  if (!command) throw new Error('Usage: node run_with_bind_config.cjs COMMAND [ARGS...]');
  const child = spawn(command, args, {
    stdio: 'inherit', env: { ...process.env, WIRE_TEST_PORTS: ports.join(' ') }
  });
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
  child.on('error', error => { console.error(error); process.exitCode = 1; });
  child.on('exit', code => { process.exitCode = code === null ? 1 : code; });
}
main().catch(error => { console.error(error); process.exitCode = 1; });
