# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A protoc plugin (`protoc-gen-solana`) that generates Rust protobuf encode/decode modules from proto3 definitions, optimized for Solana programs. Written in TypeScript, it follows the standard protoc plugin protocol: reads a serialized `CodeGeneratorRequest` from stdin, generates Rust source files, and writes a serialized `CodeGeneratorResponse` to stdout.

## Build & Development Commands

```bash
pnpm install              # Install dependencies (requires pnpm 10.30.2, Node >=24)
pnpm build                # Compile TypeScript → lib/
pnpm bundle               # Bundle with esbuild → dist/bundle/protoc-gen-solana.mjs
pnpm dist                 # Full production build: compile + bundle + pkg binary
pnpm dev                  # Watch mode (concurrent build + bundle)
pnpm format               # Format with prettier
pnpm generate:test        # Build dist, then run protoc with plugin against tests/protos/*.proto
pnpm clean                # Remove lib/ and dist/
```

There is no unit test runner for the TypeScript code. Testing is done via `pnpm generate:test`, which runs the full plugin through protoc and outputs generated Rust files to `dist/tests/generated/`. The Rust runtime (`rs/protobuf_runtime.rs`) has its own `#[cfg(test)]` unit tests runnable with `cargo test`.

## Architecture

### Execution Flow

1. **`src/index.ts`** — Entry point. Reads stdin buffer, calls `runPlugin()`, writes response to stdout. Diagnostics go to stderr.
2. **`src/plugin.ts`** — Core protocol handler. Defines the protobuf plugin schema programmatically using `protobufjs` (no `.proto` files needed at runtime). Decodes the request, extracts `MessageDescriptor` trees from proto file descriptors, generates Rust files, encodes the response.
3. **`src/generator/`** — Code generation:
   - **`message.ts`** — Generates Rust struct definitions and `impl` blocks with `encode()`/`decode()` methods per message. Orchestrates enum + struct emission into each `.rs` file.
   - **`enum.ts`** — Generates Rust `#[repr(i32)]` enum definitions with `Default`, `From<i32>`, and `Into<i32>` impls.
   - **`field.ts`** — Field-level encode/decode logic. Handles scalars, nested messages, enums, repeated fields, and maps.
   - **`type-map.ts`** — Maps protobuf field type enum values (1–18) to Rust types, wire types, and runtime function names. Central reference for type resolution.
   - **`runtime.ts`** — Loads `rs/protobuf_runtime.rs` from disk and emits it as an output file.
4. **`src/util/`** — `names.ts` converts proto names to Rust conventions (PascalCase structs, snake_case fields). `logger.ts` wraps `tracer` for stderr-only logging.
5. **`rs/protobuf_runtime.rs`** — Rust runtime library emitted alongside generated code. Provides all wire format primitives (varint, fixed, zigzag, length-delimited, bool). Embedded into the pkg binary via the `pkg.assets` config.

### Key Design Decisions

- **Maps → parallel Vecs**: Proto map fields become `Vec<K>` + `Vec<V>` pairs rather than `HashMap`, for efficient Solana serialization.
- **Self-contained plugin protocol**: The protobuf schema for `CodeGeneratorRequest`/`CodeGeneratorResponse` is defined programmatically in `plugin.ts`, not loaded from `.proto` files.
- **Borsh integration**: Generated structs derive `borsh::BorshSerialize` and `borsh::BorshDeserialize` (feature-gated).
- **Enum generation**: Proto enums generate `#[repr(i32)]` Rust enums with `Default`, `From<i32>`, and `From<Enum> for i32` impls. Struct fields use the named enum type; wire encode/decode uses varint via `as i32`/`From<i32>` casts. Both top-level and message-nested enums are supported.
- **Varint casting**: The type system tracks which types need `as u64`/`as T` casts for varint encode/decode since the runtime always works with `u64`.

### Build Pipeline

TypeScript compiles to `lib/`, esbuild bundles to `dist/bundle/protoc-gen-solana.mjs` (ESM with shebang), and `@yao-pkg/pkg` produces a standalone binary at `dist/bin/protoc-gen-solana` that embeds the `rs/` directory as assets.
