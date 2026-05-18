import "jest"
import Fs from "node:fs"
import Path from "node:path"

import * as Sh from "shelljs"

const pkgRootPath = Path.resolve(__dirname, ".."),
  fixturePath = Path.join(__dirname, "fixtures", "hello_world"),
  outPath = Path.join(pkgRootPath, "out", "tests", "hello_world"),
  pluginFile = Path.join(pkgRootPath, "dist", "bundle", "protoc-gen-solana.cjs")

namespace IntegrationTest {
  export const SetupTimeoutMs = 30_000
  export const CargoTestTimeoutMs = 120_000
}

/** Derived paths inside the working copy (not the fixture). */
const outProtoPath = Path.join(outPath, "protos"),
  outRustSrcPath = Path.join(outPath, "src")

const generatedFiles = [
  Path.join(outRustSrcPath, "protobuf_runtime.rs"),
  Path.join(outRustSrcPath, "hello", "hello_world.rs"),
  Path.join(outRustSrcPath, "hello", "types", "sample_types.rs")
]

/**
 * Resolve a binary path via `which`, returning undefined when absent.
 */
const tryWhich = (bin: string): string | undefined =>
  Sh.which(bin) ?? undefined

// ── Preflight ───────────────────────────────────────────────────────

let cargoPath: string | undefined,
  npxPath: string | undefined,
  setupError: string | undefined

beforeAll(async () => {
  cargoPath = tryWhich("cargo")
  npxPath = tryWhich("npx")

  if (!cargoPath) {
    setupError =
      "cargo not found — Rust toolchain is required for this integration test"
    return
  }
  if (!npxPath) {
    setupError =
      "npx not found — Node toolchain is required for this integration test"
    return
  }
  if (!Fs.existsSync(pluginFile)) {
    setupError = `plugin bundle not found at ${pluginFile} — run 'pnpm build' first`
    return
  }

  // Start fresh: remove previous output, copy immutable fixture
  Sh.rm("-rf", outPath)
  Sh.mkdir("-p", outPath)
  Sh.cp("-R", Path.join(fixturePath, "*"), outPath)
  Sh.cp(Path.join(fixturePath, ".gitignore"), outPath)

  // Ensure generated output directories exist
  Sh.mkdir("-p", Path.join(outRustSrcPath, "hello", "types"))

  // Generate Rust from protos via npx protoc
  const result = Sh.exec(
    [
      "npx protoc",
      `--plugin=protoc-gen-solana=${pluginFile}`,
      `--solana_out=${outRustSrcPath}`,
      `--proto_path=${outProtoPath}`,
      "types/sample_types.proto",
      "hello_world.proto"
    ].join(" ")
  )

  if (result.code !== 0) {
    setupError = `protoc failed (exit ${result.code}): ${result.stderr}`
  }
}, IntegrationTest.SetupTimeoutMs)

// ── Tests ───────────────────────────────────────────────────────────

describe("hello_world integration (protoc → Rust → cargo test)", () => {
  beforeEach(() => {
    if (setupError) {
      console.warn(`SKIPPED: ${setupError}`)
    }
  })

  it("required toolchains are available", () => {
    expect(setupError).toBeUndefined()
    expect(cargoPath).toBeDefined()
    expect(npxPath).toBeDefined()
    expect(Fs.existsSync(pluginFile)).toBe(true)
  })

  it("generated all expected .rs files", () => {
    if (setupError) return

    generatedFiles.forEach(f => {
      expect(Fs.existsSync(f)).toBe(true)
    })
  })

  it("sample_types.rs contains enum definitions and impls", () => {
    if (setupError) return

    const src = Fs.readFileSync(
      Path.join(outRustSrcPath, "hello", "types", "sample_types.rs"),
      "utf-8"
    )

    // Enum type definitions with repr
    expect(src).toContain("#[repr(i32)]")
    expect(src).toContain("pub enum Priority {")
    expect(src).toContain("pub enum Status {")

    // Enum variants
    expect(src).toContain("PriorityUnspecified = 0,")
    expect(src).toContain("PriorityCritical = 4,")
    expect(src).toContain("StatusFailed = 4,")

    // Trait impls
    expect(src).toContain("impl Default for Priority")
    expect(src).toContain("impl From<i32> for Priority")
    expect(src).toContain("impl From<Priority> for i32")

    // Struct definitions
    expect(src).toContain("pub struct TaggedId {")
    expect(src).toContain("pub struct Metadata {")

    // Enum field uses the named type, not i32
    expect(src).toContain("pub priority: Priority,")
  })

  it("hello_world.rs has cross-file imports and local enum", () => {
    if (setupError) return

    const src = Fs.readFileSync(
      Path.join(outRustSrcPath, "hello", "hello_world.rs"),
      "utf-8"
    )

    // Cross-file import for hello.types package
    expect(src).toContain("use crate::hello::types::types::*;")

    // Local enum definition
    expect(src).toContain("pub enum Greeting {")
    expect(src).toContain("GreetingHello = 1,")

    // Structs referencing both local and cross-file types
    expect(src).toContain("pub struct HelloRequest {")
    expect(src).toContain("pub struct HelloResponse {")

    // Enum fields use named types
    expect(src).toContain("pub greeting: Greeting,")
    expect(src).toContain("pub priority: Priority,")
    expect(src).toContain("pub status: Status,")
  })

  it(
    "cargo test passes: all encode/decode round-trips succeed",
    async () => {
      if (setupError) return

      const result = Sh.exec("cargo test -- --nocapture", { cwd: outPath })
      const output = result.stdout + result.stderr

      expect(output).toContain("test result: ok")
      expect(output).toContain("enum_default_is_zero_variant")
      expect(output).toContain("hello_request_full_roundtrip")
      expect(output).toContain("hello_response_roundtrip")
      expect(output).toContain("tagged_id_encode_decode_roundtrip")
      expect(output).toContain(
        "nested_enum_field_preserved_through_parent_encode"
      )
    },
    IntegrationTest.CargoTestTimeoutMs
  )
})
