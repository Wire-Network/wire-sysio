import Yargs from "yargs"
import Path from "path"
import Fs from "fs"
import { hideBin } from "yargs/helpers"
import { log, setLogLevel } from "./util/logger.js"
import { bundleCommand } from "./commands/bundle.js"
import { resolveSynchronizedVersion } from "./util/resolveVersion.js"
import {
  Target,
  ALL_TARGETS,
  PUBLISHABLE_TARGETS,
  TARGET_BUILD_ORDER
} from "./constants.js"
import Assert from "assert"

let RootDir: string = Path.resolve(__dirname)
while (RootDir.length) {
  if (Fs.existsSync(Path.join(RootDir,".git"))) {
    log.info("Resolved repo root: %s", RootDir)
    break
  }
  RootDir = Path.dirname(RootDir)
}

Assert.ok(Fs.existsSync(Path.join(RootDir,".git")), `Could not find repo root from __dirname: ${__dirname}`)

namespace Defaults {
  export const ProtoPath = Path.join(RootDir, "libraries","opp","proto")

  Assert.ok(Fs.existsSync(ProtoPath), `Default proto path does not exist: ${ProtoPath}`)


  export const OutputPath = Path.join(RootDir, "build","opp-packages")

}


async function main(): Promise<void> {
  const argv = await Yargs(hideBin(process.argv))
    .scriptName("wire-protobuf-bundler")
    .usage(
      "$0 [--output <dir>] [--repo <repo>] [--output <dir2>] [--target <target>]"
    )
    .option("repo", {
      type: "string",
      demandOption: true,
      default: "file://" + Defaults.ProtoPath,
      describe:
        "GitHub repo spec '<owner/repo>[/<subfolder>][#<branch>]' or local path 'file://<path>'"
    })
    .option("target", {
      type: "string",
      choices: ALL_TARGETS,
      describe:
        "Code generation target. When omitted, all targets are built."
    })
    .option("output", {
      type: "string",
      array: true,
      demandOption: true,
      default: [Defaults.OutputPath],
      describe:
        "Base output directory (repeatable). Packages are written to <output>/<target>/ in each."
    })
    .option("package-version", {
      type: "string",
      describe:
        "Semver version. If omitted, resolved from npm (ts/solidity synced, solana defaults to 0.1.0)."
    })
    .option("publish", {
      type: "boolean",
      default: false,
      describe:
        "Publish typescript/solidity packages to npm after generation."
    })
    .option("verbose", {
      type: "boolean",
      default: false,
      describe: "Enable debug logging"
    })
    .example(
      "$0 --repo 'Wire-Network/wire-sysio/libraries/opp#feature/protobuf-support-for-opp' --output build/generated",
      "Generate all targets into a single output directory"
    )
    .example(
      "$0 --repo 'file:///local/path' --target typescript --output /tmp/out1 --output /tmp/out2",
      "Generate typescript into multiple output directories"
    )
    .strict()
    .help()
    .parse()

  if (argv.verbose) {
    setLogLevel("debug")
  }

  log.info("protobuf-bundler starting")

  // Determine which targets to build
  const requestedTargets: Target[] = argv.target
    ? [argv.target as Target]
    : [...ALL_TARGETS]

  // Sort into canonical build order
  const targets = TARGET_BUILD_ORDER.filter(t =>
    requestedTargets.includes(t)
  )

  // Resolve version — synchronized for ts/solidity if any are in the set
  const hasNpmTargets = targets.some(t => PUBLISHABLE_TARGETS.includes(t)),
    packageVersion =
      argv.packageVersion ??
      (hasNpmTargets ? resolveSynchronizedVersion() : "0.1.0")

  await bundleCommand({
    repo: argv.repo,
    targets,
    outputDirs: argv.output,
    packageVersion,
    publish: argv.publish
  })
}

main().catch(err => {
  log.error("Fatal: %s", err.message)
  process.exit(1)
})
