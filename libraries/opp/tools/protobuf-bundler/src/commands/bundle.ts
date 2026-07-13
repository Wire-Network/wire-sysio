// noinspection ExceptionCaughtLocallyJS

import { execFileSync, execSync } from "node:child_process"
import Crypto from "node:crypto"
import Fs from "node:fs"
import Path from "node:path"
import Os from "node:os"
import { match } from "ts-pattern"
import { log } from "../util/logger.js"
import { fetchProtos } from "../steps/fetchProtos.js"
import { runProtoc } from "../steps/runProtoc.js"
import { generatePackage } from "../steps/generatePackage.js"
import { generateTypescript } from "../steps/generateTypescript.js"
import { Target, PUBLISHABLE_TARGETS } from "../constants.js"

const PROTO_DIR_NAME = "proto"
const PROTO_FILE_EXTENSION = ".proto"
const SHA256_ALGORITHM = "sha256"
const UTF8_ENCODING = "utf-8"

export interface BundleArgs {
  repo: string
  targets: Target[]
  outputDirs: string[]
  packageVersion: string
  publish: boolean
}

let skipCleanup = false

export async function bundleCommand(args: BundleArgs): Promise<void> {
  const resolvedOutputDirs = args.outputDirs.map(d => Path.resolve(d)),
    tmpDir = Fs.mkdtempSync(Path.join(Os.tmpdir(), "protobuf-bundler-"))
  log.debug("Using temp dir: %s", tmpDir)

  try {
    const protoFiles = await fetchProtos({
        repo: args.repo,
        outputDir: tmpDir
      }),
      protoDir = Path.join(tmpDir, "proto")

    // Build each target once in staging, then distribute to all output dirs
    for (const target of args.targets) {
      log.info("Building target: %s", target)

      const stagingDir = await match(target)
        .with(Target.Solana, () =>
          buildSolanaPackage(args, tmpDir, protoFiles, protoDir)
        )
        .with(Target.Typescript, () =>
          buildTypescriptPackage(args, tmpDir, protoFiles, protoDir)
        )
        .with(Target.Solidity, () =>
          buildSolidityPackage(args, tmpDir, protoFiles, protoDir)
        )
        .exhaustive()

      // Copy staging to all output dirs in parallel, then npm i in each
      await Promise.all(
        resolvedOutputDirs.map(async baseOutputDir => {
          const targetOutputDir = Path.join(baseOutputDir, target)
          log.info("Distributing %s → %s", target, targetOutputDir)
          Fs.mkdirSync(targetOutputDir, { recursive: true })
          copyDirExcluding(
            stagingDir,
            targetOutputDir,
            new Set(["node_modules"])
          )

          // Solana doesn't need npm i
          if (target !== Target.Solana) {
            log.info("Installing dependencies in %s", targetOutputDir)
            execSync("npm i", {
              cwd: targetOutputDir,
              encoding: "utf-8",
              stdio: ["pipe", "pipe", "inherit"]
            })
          }
        })
      )
    }

    validateBundledProtoProvenance(protoDir, resolvedOutputDirs, args.targets)

    log.info("Bundle complete → %s", resolvedOutputDirs.join(", "))

    if (args.publish) {
      // Publish from the first output dir only
      await handlePublish(args, resolvedOutputDirs[0])
    }
  } catch (err: any) {
    skipCleanup = true
    log.error(`Bundle failed: ${err.message}`, err)
    // Re-throw so the failure propagates to `main().catch` → process.exit(1).
    // Without this the CLI exits 0 on a failed generate/publish and CI reports
    // a green run even though no package was produced or published.
    throw err
  } finally {
    if (!skipCleanup) {
      try {
        Fs.rmSync(tmpDir, { recursive: true, force: true })
        log.debug("Cleaned up temp dir: %s", tmpDir)
      } catch (err: any) {
        log.warn("Failed to clean temp dir %s: %s", tmpDir, err.message)
      }
    }
  }
}

// ─── Per-target builders (return staging dir path) ──────────────────────────

async function buildSolanaPackage(
  args: BundleArgs,
  tmpDir: string,
  protoFiles: string[],
  protoDir: string
): Promise<string> {
  const stagingDir = Path.join(tmpDir, "staging-solana")
  Fs.mkdirSync(stagingDir, { recursive: true })

  const generatedFiles = await runProtoc({
      target: Target.Solana,
      protoFiles,
      protoDir,
      outputDir: tmpDir
    }),
    genDir = Path.join(tmpDir, "generated")

  await generatePackage({
    target: Target.Solana,
    outputDir: stagingDir,
    packageVersion: args.packageVersion,
    generatedFiles,
    genDir,
    repo: args.repo
  })

  copyProtoSources(protoFiles, protoDir, stagingDir)

  return stagingDir
}

async function buildTypescriptPackage(
  args: BundleArgs,
  tmpDir: string,
  protoFiles: string[],
  protoDir: string
): Promise<string> {
  const stagingDir = Path.join(tmpDir, "staging-typescript")
  Fs.mkdirSync(stagingDir, { recursive: true })

  await generatePackage({
    target: Target.Typescript,
    outputDir: stagingDir,
    packageVersion: args.packageVersion,
    generatedFiles: [],
    genDir: Path.join(tmpDir, "generated"),
    repo: args.repo
  })

  copyProtoSources(protoFiles, protoDir, stagingDir)

  await generateTypescript({
    target: Target.Typescript,
    protoFiles,
    protoDir,
    tmpDir,
    outputDir: stagingDir
  })

  await installAndCompile(stagingDir)

  return stagingDir
}

async function buildSolidityPackage(
  args: BundleArgs,
  tmpDir: string,
  protoFiles: string[],
  protoDir: string
): Promise<string> {
  const stagingDir = Path.join(tmpDir, "staging-solidity")
  Fs.mkdirSync(stagingDir, { recursive: true })

  const generatedFiles = await runProtoc({
      target: Target.Solidity,
      protoFiles,
      protoDir,
      outputDir: tmpDir
    }),
    genDir = Path.join(tmpDir, "generated")

  await generatePackage({
    target: Target.Solidity,
    outputDir: stagingDir,
    packageVersion: args.packageVersion,
    generatedFiles,
    genDir,
    repo: args.repo
  })

  copyProtoSources(protoFiles, protoDir, stagingDir)

  await generateTypescript({
    target: Target.Solidity,
    protoFiles,
    protoDir,
    tmpDir,
    outputDir: stagingDir
  })

  await installAndCompile(stagingDir)

  return stagingDir
}

// ─── Helpers ────────────────────────────────────────────────────────────────

async function installAndCompile(stagingDir: string): Promise<void> {
  log.info("Installing dependencies in staging dir…")
  execSync("npm i", {
    cwd: stagingDir,
    encoding: "utf-8",
    stdio: ["pipe", "pipe", "inherit"]
  })

  log.info("Compiling TypeScript in %s", stagingDir)
  execFileSync(
    "npx",
    [
      "-y",
      "-p",
      "typescript@4",
      "tsc",
      "-b",
      Path.join(stagingDir, "tsconfig.json")
    ],
    {
      stdio: ["pipe", "pipe", "inherit"],
      cwd: stagingDir
    }
  )

  log.info("Fixing import extensions in %s", stagingDir)
  Array("tsconfig.cjs.json", "tsconfig.esm.json")
    .map(tsConfigFileName => Path.join(stagingDir, tsConfigFileName))
    .forEach(tsConfigPath => {
      execFileSync(
        "npx",
        [
          "-y",
          "-p",
          "tsc-alias",
          "tsc-alias",
          "-p",
          tsConfigPath,
          "-f",
          "-fe",
          ".js"
        ],
        {
          stdio: ["pipe", "pipe", "inherit"],
          cwd: stagingDir
        }
      )
    })
}

function copyProtoSources(
  protoFiles: string[],
  protoDir: string,
  outputDir: string
): void {
  const protoOutDir = Path.join(outputDir, PROTO_DIR_NAME)
  Fs.mkdirSync(protoOutDir, { recursive: true })
  protoFiles.forEach(pf => {
    const relative = Path.relative(protoDir, pf),
      dest = Path.join(protoOutDir, relative)
    Fs.mkdirSync(Path.dirname(dest), { recursive: true })
    Fs.copyFileSync(pf, dest)
  })
}

interface ProtoManifestEntry {
  relativePath: string
  sha256: string
}

/**
 * Verify every generated package carries the exact proto sources used as the
 * code-generation input before publish can run.
 */
function validateBundledProtoProvenance(
  sourceProtoDir: string,
  outputDirs: string[],
  targets: Target[]
): void {
  const expectedManifest = buildProtoManifest(sourceProtoDir),
    expectedJson = JSON.stringify(expectedManifest),
    expectedDigest = digestManifest(expectedManifest)

  outputDirs.forEach(baseOutputDir => {
    targets.forEach(target => {
      const targetProtoDir = Path.join(baseOutputDir, target, PROTO_DIR_NAME),
        actualManifest = buildProtoManifest(targetProtoDir),
        actualJson = JSON.stringify(actualManifest)

      if (actualJson !== expectedJson) {
        throw new Error(
          `Generated ${target} package proto provenance does not match source protos in ${targetProtoDir}`
        )
      }

      log.info(
        "Verified %s proto provenance in %s (%d files, sha256:%s)",
        target,
        targetProtoDir,
        expectedManifest.length,
        expectedDigest
      )
    })
  })
}

/**
 * Build a stable manifest of .proto files and their SHA-256 digests.
 */
function buildProtoManifest(protoDir: string): ProtoManifestEntry[] {
  if (!Fs.existsSync(protoDir)) {
    throw new Error(`Proto directory does not exist: ${protoDir}`)
  }

  return walkFiles(protoDir)
    .filter(file => file.endsWith(PROTO_FILE_EXTENSION))
    .map(file => ({
      relativePath: Path.relative(protoDir, file).split(Path.sep).join("/"),
      sha256: hashFile(file)
    }))
    .sort((a, b) => a.relativePath.localeCompare(b.relativePath))
}

/**
 * Hash a file for proto provenance comparisons.
 */
function hashFile(file: string): string {
  return Crypto.createHash(SHA256_ALGORITHM)
    .update(Fs.readFileSync(file))
    .digest("hex")
}

/**
 * Hash a manifest for compact CI logs and reviewer provenance checks.
 */
function digestManifest(manifest: ProtoManifestEntry[]): string {
  return Crypto.createHash(SHA256_ALGORITHM)
    .update(JSON.stringify(manifest), UTF8_ENCODING)
    .digest("hex")
}

/**
 * List files recursively in deterministic order.
 */
function walkFiles(dir: string): string[] {
  return Fs.readdirSync(dir, { withFileTypes: true })
    .sort((a, b) => a.name.localeCompare(b.name))
    .flatMap(entry => {
      const fullPath = Path.join(dir, entry.name)
      return entry.isDirectory() ? walkFiles(fullPath) : [fullPath]
    })
}

function publishPackage(dir: string): void {
  log.info("Publishing package from %s…", dir)
  try {
    const result = execSync("npm publish --access public", {
      cwd: dir,
      encoding: "utf-8",
      stdio: ["pipe", "pipe", "pipe"]
    })
    log.info("Published successfully: %s", result.trim())
  } catch (err: any) {
    const stderr: string = err.stderr?.toString() ?? ""
    throw new Error(`npm publish failed: ${stderr || err.message}`)
  }
}

async function handlePublish(
  args: BundleArgs,
  baseOutputDir: string
): Promise<void> {
  args.targets
    .filter(t => PUBLISHABLE_TARGETS.includes(t))
    .forEach(target => {
      publishPackage(Path.join(baseOutputDir, target))
    })
}

function copyDirExcluding(
  src: string,
  dest: string,
  exclude: Set<string>
): void {
  Fs.mkdirSync(dest, { recursive: true })
  Fs.readdirSync(src, { withFileTypes: true })
    .filter(entry => !exclude.has(entry.name))
    .forEach(entry => {
      const srcPath = Path.join(src, entry.name),
        destPath = Path.join(dest, entry.name)
      if (entry.isDirectory()) {
        copyDirExcluding(srcPath, destPath, exclude)
      } else {
        Fs.copyFileSync(srcPath, destPath)
      }
    })
}
