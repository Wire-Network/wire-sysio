import Fs from "node:fs"
import Path from "node:path"
import { SOL_PRAGMA, SPDX_LICENSE } from "../util/names.js"

/**
 * Returns the complete ProtobufRuntime.sol source.
 *
 * This is a hand-optimized library with inline assembly for hot-path
 * varint encode/decode. Emitted as a CodeGeneratorResponse file alongside
 * the per-message codecs.
 */
export function generateRuntime(): string {
  return RUNTIME_SOL
}

const
  RUNTIME_SOL_PATH = [Path.join(__dirname, "../../sol/ProtobufRuntime.sol"), Path.join(__dirname, "../sol/ProtobufRuntime.sol")].find(p => Fs.existsSync(p)),
  RUNTIME_SOL = Fs.readFileSync(RUNTIME_SOL_PATH, "utf-8")
