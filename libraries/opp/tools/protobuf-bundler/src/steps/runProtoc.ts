import { execFileSync, execSync } from "node:child_process"
import Path from "node:path"
import Fs from "node:fs"
import { asOption } from "@3fv/prelude-ts"
import { log } from "../util/logger.js"
import { Target } from "../constants.js"

export { Target }

export interface RunProtocOptions {
  target: Target
  protoFiles: string[]
  protoDir: string
  outputDir: string
}

export interface PluginSetup {
  pkg: string
  bin: string
  outFlag: string
}

const PLUGIN_MAP: Partial<Record<Target, PluginSetup>> = {
  [Target.Solana]: {
    pkg: "@wireio/protoc-gen-solana",
    bin: "protoc-gen-solana",
    outFlag: "--solana_out"
  },
  [Target.Solidity]: {
    pkg: "@wireio/protoc-gen-solidity",
    bin: "protoc-gen-solidity",
    outFlag: "--solidity_out"
  }
}

/**
 * Fallback resolution: check system PATH then node_modules/.bin.
 * Eagerly evaluated so it can be passed to `getOrElse`.
 */
function resolveFromPathOrBin(name: string, npmPkg: string): string {
  try {
    const result = execSync(`which ${name}`, { stdio: "pipe" })
      .toString()
      .trim()
    if (result) return result
  } catch {
    // not on PATH
  }

  const localBin = Path.join("node_modules", ".bin", name)
  if (Fs.existsSync(localBin)) {
    return Path.resolve(localBin)
  }

  throw new Error(
    `Plugin binary "${name}" not found. Install ${npmPkg} or ensure ${name} is on PATH.`
  )
}

/**
 * Resolve a plugin binary. Search order:
 *   1. pkg binary inside the installed npm package (dist/bin/<name>)
 *   2. System PATH
 *   3. node_modules/.bin wrapper (fallback)
 */
export function resolvePluginBin(name: string, npmPkg: string): string {
  const pkgBinCandidates = [
    Path.join("node_modules", ".bin", name),
    Path.join("node_modules", npmPkg, "dist", "bin", name),
    Path.join(
      "node_modules",
      ".pnpm",
      "node_modules",
      npmPkg,
      "dist",
      "bin",
      name
    )
  ]

  return asOption(
    pkgBinCandidates.find(candidate => Fs.existsSync(candidate))
  )
    .map(candidate => {
      const resolved = Path.resolve(candidate)
      log.debug("Found pkg binary at: %s", resolved)
      return resolved
    })
    .getOrElse(resolveFromPathOrBin(name, npmPkg))
}

/**
 * Determine the best --proto_path root. Proto files use import paths
 * relative to a root directory. If the cloned content has a "proto/" or
 * "protos/" subdirectory, that is likely the import root.
 */
export function findProtoRoot(baseDir: string): string {
  return asOption(
    ["proto", "protos"]
      .map(candidate => Path.join(baseDir, candidate))
      .find(dir => Fs.existsSync(dir) && Fs.statSync(dir).isDirectory())
  )
    .tap(dir => log.debug("Found proto root subdirectory: %s", dir))
    .getOrElse(baseDir)
}

export async function runProtoc(opts: RunProtocOptions): Promise<string[]> {
  const pluginInfo = PLUGIN_MAP[opts.target]
  if (!pluginInfo) {
    throw new Error(
      `No protoc plugin configured for target "${opts.target}"`
    )
  }

  const pluginBin = resolvePluginBin(pluginInfo.bin, pluginInfo.pkg)
  log.debug("Resolved plugin binary: %s → %s", pluginInfo.bin, pluginBin)

  const genDir = Path.join(opts.outputDir, "generated")
  Fs.mkdirSync(genDir, { recursive: true })

  const protoRoot = findProtoRoot(opts.protoDir)

  const relativeProtos = opts.protoFiles.map(p =>
    Path.relative(protoRoot, p)
  )

  const args = [
    `--plugin=${pluginInfo.bin}=${pluginBin}`,
    `--proto_path=${protoRoot}`,
    `${pluginInfo.outFlag}=${genDir}`,
    ...relativeProtos
  ]

  log.info("Running: npx protoc %s", args.join(" "))

  try {
    execFileSync("npx", ["protoc", ...args], {
      stdio: ["pipe", "pipe", "inherit"]
    })
  } catch (err: any) {
    throw new Error(
      `protoc failed (exit ${err.status}): ${err.stderr?.toString() ?? err.message}`
    )
  }

  const generated = walkDir(genDir)
  log.info("protoc generated %d file(s)", generated.length)
  return generated
}

function walkDir(dir: string): string[] {
  if (!Fs.existsSync(dir)) return []
  return Fs.readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const fullPath = Path.join(dir, entry.name)
    return entry.isDirectory() ? walkDir(fullPath) : [fullPath]
  })
}
