import { execSync } from "node:child_process"
import { asOption } from "@3fv/prelude-ts"
import { log } from "./logger.js"
import {
  Channel,
  DEV_PRERELEASE_SUFFIX,
  Target,
  TargetPackageName,
  PUBLISHABLE_TARGETS
} from "../constants.js"

/**
 * Query npm for the latest published version of a package.
 * Returns undefined if the package is not published (E404).
 */
export function queryNpmVersion(packageName: string): string | undefined {
  try {
    return execSync(`npm show ${packageName} version`, {
      encoding: "utf-8",
      stdio: ["pipe", "pipe", "pipe"]
    }).trim()
  } catch (err: any) {
    const stderr: string = err.stderr?.toString() ?? ""
    if (
      stderr.includes("E404") ||
      stderr.includes("is not in this registry")
    ) {
      return undefined
    }
    throw new Error(
      `Failed to query npm for "${packageName}": ${err.message}`
    )
  }
}

/**
 * Query npm for the latest published version of a package,
 * then return that version with the patch number incremented by 1.
 *
 * Throws if the package exists but the version cannot be determined.
 */
export function resolveNextVersion(packageName: string): string {
  log.info("Resolving latest version of %s from npm…", packageName)

  const raw = queryNpmVersion(packageName)

  if (raw === undefined) {
    throw new Error(
      `Package "${packageName}" not found on npm. ` +
        `Cannot auto-resolve version — please supply --package-version explicitly.`
    )
  }

  if (!raw) {
    throw new Error(
      `npm returned an empty version string for "${packageName}". ` +
        `Cannot auto-resolve version — please supply --package-version explicitly.`
    )
  }

  const m = raw.match(/^(\d+)\.(\d+)\.(\d+)/)
  if (!m) {
    throw new Error(
      `npm returned an unparseable version "${raw}" for "${packageName}". ` +
        `Cannot auto-resolve version — please supply --package-version explicitly.`
    )
  }

  const next = `${m[1]}.${m[2]}.${parseInt(m[3], 10) + 1}`
  log.info("Current version: %s → next version: %s", raw, next)
  return next
}

/**
 * For the typescript/solidity pair: query all publishable package versions
 * from npm, take the greatest semver, increment patch, and return that
 * version. This ensures both packages always publish at the same version.
 *
 * @param channel optional release channel — `Channel.dev` appends the
 *   prerelease suffix to the resolved version (`X.Y.Z-dev`);
 *   `Channel.stable` or omitted returns the bare version. The resolution
 *   itself (latest published → patch bump) is identical for every channel;
 *   the parse below already reads the `X.Y.Z` core out of a suffixed latest
 *   (`1.0.40-dev` → next `1.0.41[-dev]`), so consecutive dev publishes bump
 *   normally.
 */
export function resolveSynchronizedVersion(channel?: Channel): string {
  log.info("Resolving synchronized version for publishable targets…")

  const versions = PUBLISHABLE_TARGETS.map(target => {
    const name = TargetPackageName[target],
      version = queryNpmVersion(name)
    log.info(
      "  %s: %s",
      name,
      version ?? "(not published)"
    )
    return version
  }).filter((v): v is string => v !== undefined)

  return asOption(versions)
    .filter(vs => vs.length > 0)
    .map(vs =>
      vs.reduce((a, b) => {
        const [aMaj, aMin, aPat] = a.split(".").map(Number)
        const [bMaj, bMin, bPat] = b.split(".").map(Number)
        return aMaj !== bMaj
          ? aMaj > bMaj
            ? a
            : b
          : aMin !== bMin
            ? aMin > bMin
              ? a
              : b
            : aPat >= bPat
              ? a
              : b
      })
    )
    .map(maxVersion => {
      const m = maxVersion.match(/^(\d+)\.(\d+)\.(\d+)/)!
      const core = `${m[1]}.${m[2]}.${parseInt(m[3], 10) + 1}`,
        next =
          channel === Channel.dev ? `${core}-${DEV_PRERELEASE_SUFFIX}` : core
      log.info("Synchronized next version: %s", next)
      return next
    })
    .getOrThrow(
      `No publishable packages found on npm. ` +
        `Cannot auto-resolve version — please supply --package-version explicitly.`
    )
}
