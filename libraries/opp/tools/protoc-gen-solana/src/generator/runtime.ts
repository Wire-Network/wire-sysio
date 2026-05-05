import Fs from "node:fs"
import Path from "node:path"

/**
 * Returns the complete protobuf_runtime.rs source.
 *
 * This is a Rust library with efficient protobuf3 wire format
 * encode/decode primitives optimized for Solana's compute budget.
 * Emitted as a CodeGeneratorResponse file alongside the per-message codecs.
 */
export function generateRuntime(): string {
  return RUNTIME_RS
}

const
  RUNTIME_RS_PATH = [Path.join(__dirname, "../../rs/protobuf_runtime.rs"), Path.join(__dirname, "../rs/protobuf_runtime.rs")].find(p => Fs.existsSync(p)),
  RUNTIME_RS = Fs.readFileSync(RUNTIME_RS_PATH, "utf-8")
