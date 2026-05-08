import Fs from "node:fs"
import Path from "node:path"
import { log } from "./logger.js"
import Assert from "node:assert"

export function resolveCandidatePath(
  dirname: string,
  ...candidates: string[]
): string {
  const candidatePaths = candidates.map(c => Path.join(dirname, c))
  const c = candidatePaths.find(p => Fs.existsSync(p))
  if (!c) {
    throw new Error(`No candidate found: ${candidatePaths.join(", ")}`)
  }
  return c
}

/**
 * Checks whether a file or directory exists at the given path.
 *
 * @param path - Absolute or relative path to check.
 * @returns `true` if the path is accessible, `false` otherwise.
 */
export async function exists(path: string): Promise<boolean> {
  try {
    await Fs.promises.access(path, Fs.constants.F_OK)
    return true
  } catch {
    return false
  }
}
/**
 * Removes a symbolic-link directory at the given path.
 *
 * If the path does not exist, returns `true` immediately. If it exists and is
 * both a directory and a symbolic link, it is unlinked. When the initial unlink
 * fails, a forced recursive delete is attempted as a fallback.
 *
 * @param path - Absolute or relative path to the symlink directory to remove.
 * @returns `true` if the path no longer exists after the operation, `false` otherwise.
 */
export async function removeSymLinkDirectory(path: string): Promise<boolean> {
  if (!(await exists(path))) return true

  try {
    const stats = await Fs.promises.lstat(path)
    Assert.ok(stats, "Failed to get stats for path")

    if (stats.isDirectory() && stats.isSymbolicLink()) {
      log.info("Removing existing node_modules at %s", path)
      if (stats.isSymbolicLink()) {
        await Fs.promises.unlink(path)
        if (await exists(path)) {
          log.warn("Symlink removal failed, attempting force delete: %s", path)
          await Fs.promises.rm(path, { recursive: true, force: true })
        }
      } else {
        await Fs.promises.rmdir(path)
      }
    }
  } catch (err) {
    log.warn(`Failed to clean up node_modules at: ${path}`, err)
  }

  return !(await exists(path))
}
