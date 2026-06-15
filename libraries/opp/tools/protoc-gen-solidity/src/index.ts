import "./TypescriptFactoryShim.js"
import { log } from "./util/logger.js"
import { runPlugin } from "./plugin.js"

/**
 * protoc-gen-solidity entry point.
 *
 * protoc invokes this binary, writes a serialized CodeGeneratorRequest
 * to stdin, and reads a serialized CodeGeneratorResponse from stdout.
 * All diagnostic output goes to stderr via tracer.
 */
async function main(): Promise<void> {
  log.info("protoc-gen-solidity starting")

  const stdin = await readStdin()
  log.debug("Read %d bytes from stdin", stdin.length)

  const stdout = runPlugin(stdin)
  log.debug("Writing %d bytes to stdout", stdout.length)

  process.stdout.write(stdout, err => {
    if (err) {
      log.error("Failed to write response: %s", err.message)
      process.exit(1)
    }
  })
}

/**
 * Read all of stdin into a single Buffer.
 */
function readStdin(): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = []
    process.stdin.on("data", (chunk: Buffer) => chunks.push(chunk))
    process.stdin.on("end", () => resolve(Buffer.concat(chunks)))
    process.stdin.on("error", reject)
  })
}

main().catch(err => {
  log.error("Fatal: %s", err.message)
  process.exit(1)
})
