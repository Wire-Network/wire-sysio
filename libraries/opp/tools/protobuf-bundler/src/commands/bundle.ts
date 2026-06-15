// noinspection ExceptionCaughtLocallyJS

import { execFileSync, execSync } from "node:child_process"
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

    log.info("Bundle complete → %s", resolvedOutputDirs.join(", "))

    if (args.publish) {
      // Publish from the first output dir only
      await handlePublish(args, resolvedOutputDirs[0])
    }
  } catch (err: any) {
    skipCleanup = true
    log.error(`Bundle failed: ${err.message}`, err)
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
  const protoOutDir = Path.join(outputDir, "proto")
  Fs.mkdirSync(protoOutDir, { recursive: true })
  protoFiles.forEach(pf => {
    const relative = Path.relative(protoDir, pf),
      dest = Path.join(protoOutDir, relative)
    Fs.mkdirSync(Path.dirname(dest), { recursive: true })
    Fs.copyFileSync(pf, dest)
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
