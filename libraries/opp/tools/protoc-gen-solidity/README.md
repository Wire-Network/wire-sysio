# @wireio/protoc-gen-solidity

[![npm](https://img.shields.io/npm/v/@wireio/protoc-gen-solidity)](https://www.npmjs.com/package/@wireio/protoc-gen-solidity)

A `protoc` plugin that generates Solidity libraries with full protobuf3 wire format **encode** and **decode** support for on-chain / off-chain interoperability.

> Part of the [`wire-libraries-ts`](../../README.md) monorepo.

## Install

```bash
npm install @wireio/protoc-gen-solidity
```

Requires Node >= 24.

## Usage

```bash
npx protoc \
  --plugin=protoc-gen-solidity=./node_modules/.bin/protoc-gen-solidity \
  --solidity_out=./generated \
  path/to/your.proto
```

### Plugin Parameters

Pass via `--solidity_opt`:

```bash
npx protoc --solidity_opt=log_level=debug ...
```

| Parameter   | Values                        | Default |
|-------------|-------------------------------|---------|
| `log_level` | `trace,debug,info,warn,error` | `info`  |

## Architecture

```
.proto → protoc --plugin=protoc-gen-solidity
          ├── ProtobufRuntime.sol   (shared wire format primitives)
          └── Example.sol           (struct + codec library per message)
```

### Generated Output

Each `.proto` file produces a single `.sol` containing:

- **Struct definitions** — one per message (maps become parallel arrays)
- **Codec libraries** — `MessageNameCodec.encode(msg) → bytes` and `MessageNameCodec.decode(bytes) → msg` with tag-dispatch loop

### Runtime Library

`ProtobufRuntime.sol` provides gas-optimized wire primitives with inline assembly for varint encode/decode hot paths (~40–60% gas reduction vs pure Solidity).

## Type Mapping

| Proto | Solidity | Wire Type |
|-------|----------|-----------|
| `int32` / `int64` | `int32` / `int64` | Varint |
| `uint32` / `uint64` | `uint32` / `uint64` | Varint |
| `sint32` / `sint64` | `int32` / `int64` | Varint (ZigZag) |
| `bool` | `bool` | Varint |
| `string` | `string` | Length-delimited |
| `bytes` | `bytes` | Length-delimited |
| `fixed32` / `fixed64` | `uint32` / `uint64` | Fixed |
| `sfixed32` / `sfixed64` | `int32` / `int64` | Fixed |
| `enum` | `uint64` | Varint |
| `message` | `struct` | Length-delimited |
| `repeated T` | `T[]` | Sequential tags |
| `map<K,V>` | `K[]` + `V[]` | Length-delimited |

## Project Structure

```
src/
├── index.ts              # stdin/stdout protoc bridge
├── plugin.ts             # Request processing & descriptor walking
├── generator/
│   ├── type-map.ts       # Proto → Solidity type mapping
│   ├── field.ts          # Field-level encode/decode codegen
│   ├── enum.ts           # Enum UDVT + library generation
│   ├── message.ts        # Message-level .sol file generation
│   └── runtime.ts        # ProtobufRuntime.sol emitter
└── util/
    ├── logger.ts         # tracer-based stderr logging
    └── names.ts          # Naming convention utilities
```

## Development

```bash
pnpm install
pnpm build        # Compile TypeScript + esbuild bundle
pnpm dev          # Watch mode (compile + bundle)
pnpm dist         # Full production build (compile + bundle + pkg binary)
pnpm test         # Run unit tests
pnpm format       # Format source with prettier
pnpm clean        # Remove build artifacts
```

### Integration Testing

```bash
pnpm generate:test
```

Builds the plugin binary and runs `protoc` against the proto files in `tests/protos/`, writing generated Solidity output to `dist/tests/generated/`.

## License

MIT
