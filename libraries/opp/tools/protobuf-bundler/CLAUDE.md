# wire-protobuf-bundler

CLI tool: `protobuf-bundler`

## Build & Development

```bash
pnpm install          # Install dependencies
pnpm build            # tsc → lib/
pnpm bundle           # esbuild → dist/bundle/wire-protobuf-bundler.mjs
pnpm dist             # build + bundle + pkg → dist/bin/wire-protobuf-bundler
pnpm dev              # Watch mode (build + bundle concurrent)
pnpm format           # Prettier on src/
pnpm clean            # rm -rf lib dist
```

## Architecture

Three-step pipeline orchestrated by `src/commands/bundle.ts`:

1. **fetch-protos** — `degit` clones proto files from GitHub `<owner/repo/path#branch>`
2. **run-protoc** — `npx protoc` invoked with Wire plugin (`protoc-gen-solana` or `protoc-gen-solidity`)
3. **generate-package** — Handlebars templates render a publishable Rust crate or npm package

## Key Files

- `src/index.ts` — CLI entry (yargs)
- `src/commands/bundle.ts` — Pipeline orchestrator
- `src/steps/fetch-protos.ts` — degit download step
- `src/steps/run-protoc.ts` — protoc execution step
- `src/steps/generate-package.ts` — Template rendering + file assembly
- `src/util/logger.ts` — tracer stderr logger
- `src/util/merge.ts` — Deep merge for --package-data
- `src/util/templates.ts` — Handlebars template loader
- `templates/solana/` — Rust crate templates (.hbs)
- `templates/solidity/` — npm package templates (.hbs)

## Patterns

- ESM throughout (`"type": "module"`)
- Config mirrors `wire-protoc-gen-solana` and `wire-protoc-gen-solidity`
- Templates loaded at runtime via `fs.readFileSync` (embedded by pkg via `pkg.assets`)
- All intermediate work in OS temp dir, cleaned up on completion
