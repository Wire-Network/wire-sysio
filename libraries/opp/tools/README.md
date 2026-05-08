# Wire OPP Tools

A suite of protobuf related tools for generating reusable packages/modules with various runtime
targets including Solana (rust), Ethereum (`solidity`) and more in the future.

## Packages

| Package | Description | npm |
|---------|-------------|-----|
| [`@wireio/protoc-gen-solana`](packages/protoc-gen-solana/) | protoc plugin — Rust/Solana codegen from proto3 | [![npm](https://img.shields.io/npm/v/@wireio/protoc-gen-solana)](https://www.npmjs.com/package/@wireio/protoc-gen-solana) |
| [`@wireio/protoc-gen-solidity`](packages/protoc-gen-solidity/) | protoc plugin — Solidity codegen from proto3 | [![npm](https://img.shields.io/npm/v/@wireio/protoc-gen-solidity)](https://www.npmjs.com/package/@wireio/protoc-gen-solidity) |
| [`@wireio/wire-protobuf-bundler`](packages/protobuf-bundler/) | CLI to fetch protos and generate publishable packages | [![npm](https://img.shields.io/npm/v/@wireio/wire-protobuf-bundler)](https://www.npmjs.com/package/@wireio/wire-protobuf-bundler) |

## Requirements

- **Node.js** >= 24
- **pnpm** >= 10

## Getting Started

```bash
# Install dependencies
pnpm install

# Build all packages
pnpm build

# Run tests
pnpm test
```

## TypeScript Configuration

The project uses [project references](https://www.typescriptlang.org/docs/handbook/project-references.html) with shared base configs in `etc/tsconfig/`:

- **`tsconfig.base.json`** — ESM packages (DOM + ESNext)
- **`tsconfig.base.cjs.json`** — CommonJS packages (Node-only)
- **`tsconfig.base.jest.json`** / **`tsconfig.base.jest.json`** — Jest transforms

## License

MIT
