import Fs from "node:fs"
import Path from "node:path"
import Os from "node:os"
import { exists } from "@wireio/wire-protobuf-bundler/util/filesystemHelper"

describe("exists", () => {
  let tmpDir: string

  beforeEach(async () => {
    tmpDir = await Fs.promises.mkdtemp(Path.join(Os.tmpdir(), "pb-test-"))
  })

  afterEach(async () => {
    await Fs.promises.rm(tmpDir, { recursive: true, force: true })
  })

  it("should return true for an existing file", async () => {
    const filePath = Path.join(tmpDir, "test-file.txt")
    await Fs.promises.writeFile(filePath, "hello")
    expect(await exists(filePath)).toBe(true)
  })

  it("should return true for an existing directory", async () => {
    const dirPath = Path.join(tmpDir, "test-dir")
    await Fs.promises.mkdir(dirPath)
    expect(await exists(dirPath)).toBe(true)
  })

  it("should return false for a non-existent path", async () => {
    const missingPath = Path.join(tmpDir, "does-not-exist")
    expect(await exists(missingPath)).toBe(false)
  })

  it("should return true for a symlink to an existing target", async () => {
    const targetPath = Path.join(tmpDir, "target.txt")
    const linkPath = Path.join(tmpDir, "link.txt")
    await Fs.promises.writeFile(targetPath, "content")
    await Fs.promises.symlink(targetPath, linkPath)
    expect(await exists(linkPath)).toBe(true)
  })

  it("should return false for a broken symlink", async () => {
    const targetPath = Path.join(tmpDir, "missing-target")
    const linkPath = Path.join(tmpDir, "broken-link")
    // Create symlink to non-existent target
    await Fs.promises.symlink(targetPath, linkPath)
    expect(await exists(linkPath)).toBe(false)
  })

  it("should return true for an empty file", async () => {
    const filePath = Path.join(tmpDir, "empty.txt")
    await Fs.promises.writeFile(filePath, "")
    expect(await exists(filePath)).toBe(true)
  })
})
