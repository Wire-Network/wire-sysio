# @wireio/wire-protobuf-bundler

[![npm](https://img.shields.io/npm/v/@wireio/wire-protobuf-bundler)](https://www.npmjs.com/package/@wireio/wire-protobuf-bundler)

CLI tool that fetches `.proto` files from a GitHub repository, runs `protoc` with Wire plugins, and generates
publishable Rust crates or npm packages for OPP (On-chain Payment Protocol).

> Part of the [`wire-libraries-ts`](../../README.md) monorepo.

## Prerequisites

- **Node.js** >= 24
- **pnpm** >= 10
- **protoc** — installed via the `protoc` npm package or on PATH
- Wire protoc plugins (installed automatically as dependencies):
    - [`@wireio/protoc-gen-solana`](../protoc-gen-solana/) — for Solana/Rust target
    - [`@wireio/protoc-gen-solidity`](../protoc-gen-solidity/) — for Solidity target

## Install

```bash
npm install -g @wireio/wire-protobuf-bundler
```

Or use directly with npx:

```bash
npx @wireio/wire-protobuf-bundler --help
```

## Usage

```
wire-protobuf-bundler --repo <repo> --output <dir> [--output <dir2>] [--target <target>]
```

### Options

| Flag                | Required | Description                                                                           |
|---------------------|----------|---------------------------------------------------------------------------------------|
| `--repo`            | Yes      | GitHub repo or local path: `<owner/repo>[/<subfolder>][#<branch>]` or `file://<path>` |
| `--target`          | No       | Code generation target: `solana`, `solidity`, or `typescript`. Omit to build all.     |
| `--output`          | Yes      | Base output directory (repeatable). Packages go to `<output>/<target>/`.              |
| `--package-version` | No       | Semver version. Auto-resolved from npm for typescript/solidity.                       |
| `--publish`         | No       | Publish typescript/solidity packages to npm after generation.                         |
| `--verbose`         | No       | Enable debug logging                                                                  |

### Targets

| Target       | Package Name                    | Output                                        |
|--------------|---------------------------------|-----------------------------------------------|
| `solana`     | `wire-opp-solana-models`        | Rust crate with Cargo.toml                    |
| `solidity`   | `@wireio/opp-solidity-models`   | Hybrid npm package (Solidity contracts + TypeScript types) |
| `typescript` | `@wireio/opp-typescript-models` | npm package with TypeScript types only        |

### Examples

Build all targets into a single output directory:

```bash
wire-protobuf-bundler \
    --repo 'file://../wire-sysio/libraries/opp/proto' \
    --output build/generated
```

Build typescript into multiple output directories:

```bash
wire-protobuf-bundler \
    --repo 'file://../wire-sysio/libraries/opp/proto' \
    --target typescript \
    --output /tmp/out1 \
    --output /tmp/out2
```

Build solidity with a specific version:

```bash
wire-protobuf-bundler \
    --repo 'Wire-Network/wire-sysio/libraries/opp/proto#master' \
    --target solidity \
    --output build/generated \
    --package-version 1.0.0
```

## Pipeline

The tool executes a multi-step pipeline for each target:

1. **Fetch** — Downloads proto files from the specified repo/path using `degit` (or copies from a local `file://` path)
2. **Compile** — Runs `protoc` with the appropriate Wire plugin ([`protoc-gen-solana`](../protoc-gen-solana/) or [`protoc-gen-solidity`](../protoc-gen-solidity/))
3. **Package** — Renders Handlebars templates to produce a publishable crate or npm package
4. **Distribute** — Copies the built package to all `--output` directories in parallel

## Output Structure

### Solana target (Rust crate)

```
<output>/solana/
├── Cargo.toml
├── README.md
├── proto/                    # Original .proto source files
└── src/
    ├── lib.rs                # Barrel file re-exporting all modules
    ├── *.rs                  # Generated protobuf modules
    └── protobuf_runtime.rs   # Shared wire format primitives
```

### Solidity target (npm hybrid package)

```
<output>/solidity/
├── package.json
├── README.md
├── proto/                    # Original .proto source files
├── contracts/
│   └── *.sol                 # Generated Solidity contracts
├── src/
│   ├── index.ts              # Barrel re-exports
│   └── **/*.ts               # Generated TypeScript types
├── lib/
│   ├── cjs/                  # CommonJS output
│   └── esm/                  # ES Module output
└── tsconfig*.json
```

### Typescript target (npm package)

```
<output>/typescript/
├── package.json
├── README.md
├── proto/                    # Original .proto source files
├── src/
│   ├── index.ts              # Barrel re-exports
│   └── **/*.ts               # Generated TypeScript types
├── lib/
│   ├── cjs/                  # CommonJS output
│   └── esm/                  # ES Module output
└── tsconfig*.json
```

## Development

```bash
pnpm install
pnpm build        # TypeScript compilation
pnpm bundle       # esbuild bundling
pnpm dist         # Full build + pkg binary
pnpm dev          # Watch mode (build + bundle)
pnpm test         # Run unit tests
pnpm format       # Prettier formatting
pnpm clean        # Remove build artifacts
```

## License

MIT
