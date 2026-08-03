jest.mock("node:child_process", () => ({ execSync: jest.fn() }))

import { execSync } from "node:child_process"
import {
  queryNpmVersion,
  resolveNextVersion,
  resolveSynchronizedVersion
} from "@wireio/wire-protobuf-bundler/util/resolveVersion"
import {
  Channel,
  Target,
  TargetPackageName
} from "@wireio/wire-protobuf-bundler/constants"

const mockExecSync = execSync as jest.Mock

const TYPESCRIPT_PACKAGE = TargetPackageName[Target.Typescript]
const SOLIDITY_PACKAGE = TargetPackageName[Target.Solidity]

interface NpmCommandError extends Error {
  stderr?: Buffer
}

/** Build the error shape npm produces for an unpublished package. */
function npmNotFoundError(): NpmCommandError {
  const error: NpmCommandError = new Error("npm show failed")
  error.stderr = Buffer.from("npm ERR! code E404 - not found")
  return error
}

/**
 * Mock `npm show <pkg> version` per package: a string resolves as the
 * published version, `undefined` simulates an unpublished package (E404).
 */
function mockNpmVersions(
  byPackage: Record<string, string | undefined>
): void {
  mockExecSync.mockImplementation((command: string) => {
    const packageName = Object.keys(byPackage).find(name =>
      String(command).includes(name)
    )
    if (!packageName) {
      throw new Error(`Unexpected npm command: ${command}`)
    }
    const version = byPackage[packageName]
    if (version === undefined) {
      throw npmNotFoundError()
    }
    return `${version}\n`
  })
}

beforeEach(() => {
  mockExecSync.mockReset()
})

describe("queryNpmVersion", () => {
  it("returns the trimmed published version", () => {
    mockNpmVersions({ [TYPESCRIPT_PACKAGE]: "1.0.40" })
    expect(queryNpmVersion(TYPESCRIPT_PACKAGE)).toBe("1.0.40")
  })

  it("returns undefined for an unpublished package (E404)", () => {
    mockNpmVersions({ [TYPESCRIPT_PACKAGE]: undefined })
    expect(queryNpmVersion(TYPESCRIPT_PACKAGE)).toBeUndefined()
  })

  it("throws on a non-404 npm failure", () => {
    mockExecSync.mockImplementation(() => {
      const error: NpmCommandError = new Error("network down")
      error.stderr = Buffer.from("npm ERR! code ECONNRESET")
      throw error
    })
    expect(() => queryNpmVersion(TYPESCRIPT_PACKAGE)).toThrow(
      /Failed to query npm/
    )
  })
})

describe("resolveNextVersion", () => {
  it("bumps the patch of the latest published version", () => {
    mockNpmVersions({ [TYPESCRIPT_PACKAGE]: "2.3.7" })
    expect(resolveNextVersion(TYPESCRIPT_PACKAGE)).toBe("2.3.8")
  })

  it("throws when the package is not published", () => {
    mockNpmVersions({ [TYPESCRIPT_PACKAGE]: undefined })
    expect(() => resolveNextVersion(TYPESCRIPT_PACKAGE)).toThrow(
      /please supply --package-version/
    )
  })

  it("throws on an unparseable version", () => {
    mockNpmVersions({ [TYPESCRIPT_PACKAGE]: "garbage" })
    expect(() => resolveNextVersion(TYPESCRIPT_PACKAGE)).toThrow(
      /unparseable version/
    )
  })
})

describe("resolveSynchronizedVersion", () => {
  it("legacy no-channel path: greatest version + patch bump, bare", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: "1.0.40",
      [SOLIDITY_PACKAGE]: "1.0.40"
    })
    expect(resolveSynchronizedVersion()).toBe("1.0.41")
  })

  it("stable channel is byte-identical to the legacy path", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: "1.0.40",
      [SOLIDITY_PACKAGE]: "1.0.40"
    })
    expect(resolveSynchronizedVersion(Channel.stable)).toBe("1.0.41")
  })

  it("dev channel appends the -dev suffix after the normal bump", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: "1.0.40",
      [SOLIDITY_PACKAGE]: "1.0.40"
    })
    expect(resolveSynchronizedVersion(Channel.dev)).toBe("1.0.41-dev")
  })

  it("parses the core out of an already-suffixed latest (consecutive dev bumps)", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: "1.0.41-dev",
      [SOLIDITY_PACKAGE]: "1.0.41-dev"
    })
    expect(resolveSynchronizedVersion(Channel.dev)).toBe("1.0.42-dev")
    expect(resolveSynchronizedVersion(Channel.stable)).toBe("1.0.42")
  })

  it("takes the greatest version across the publishable pair", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: "1.1.0",
      [SOLIDITY_PACKAGE]: "1.0.9"
    })
    expect(resolveSynchronizedVersion()).toBe("1.1.1")
  })

  it("ignores an unpublished package in the pair", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: "2.1.3",
      [SOLIDITY_PACKAGE]: undefined
    })
    expect(resolveSynchronizedVersion(Channel.dev)).toBe("2.1.4-dev")
  })

  it("throws when neither package is published", () => {
    mockNpmVersions({
      [TYPESCRIPT_PACKAGE]: undefined,
      [SOLIDITY_PACKAGE]: undefined
    })
    expect(() => resolveSynchronizedVersion(Channel.dev)).toThrow(
      /please supply --package-version/
    )
  })
})
