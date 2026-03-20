# Test App Server

A browser-based test application for the `sysio.authex` contract. It connects to an Ethereum wallet (e.g. MetaMask) and creates an auth link between an Ethereum address and a Wire account.

## Prerequisites

- A running `nodeop` instance with the `sysio.authex` and `sysio.roa` contracts deployed
- A complete `sysio` build including all programs, etc (i.e. `<sysio-root>/build/debug`)
- Node.js and pnpm

## Configuration

### Private Key

The app needs a K1 private key for signing transactions. Set `AUTHEX_TEST_PRIVATE_KEY` in the `.env` file at the **repository root** (`wire-sysio/.env`):

```
AUTHEX_TEST_PRIVATE_KEY=PVT_K1_YourPrivateKeyHere
```

Webpack reads this at build time and injects it into the bundle via `DefinePlugin`.

## Account Setup

The `dev-setup.sh` script creates the on-chain accounts and resource policies needed before using the app.

```bash
./dev-setup.sh -B <build-dir> \
  --private-key <K1-private-key> \
  --link-username <username> \
  [--url <nodeop-url>]
```

### Options

| Flag | Required | Description |
|------|----------|-------------|
| `-B <build-dir>` | Yes | Path to the wire-sysio build directory (absolute, or relative to the repo root). Must contain `programs/clio/clio`. |
| `--private-key <key>` | No | A `PVT_K1_...` private key. The public key is derived from it and used for account creation. Falls back to the default dev key if omitted. |
| `--link-username <name>` | No | The Wire account name to link. **This is the username you will enter on the webpage.** |
| `--url <url>` | No | The `nodeop` URL. Defaults to `http://localhost:8888`. |

### Example

```bash
./dev-setup.sh -B build/debug-clang-clion \
  --private-key PVT_K1_<private-key> \
  --link-username john.doe
```

The script will:
1. Create the `node1` and `<link-username>` accounts on chain (if they don't already exist)
2. Register `node1` as a tier-1 node owner in `sysio.roa`
3. Add a resource policy for `<link-username>` under `node1`

## Running the Dev Server

```bash
pnpm install
pnpm run dev
```

The app will be available at **http://localhost:4000**. It proxies chain API calls to the `nodeop` instance at `http://localhost:8888`.

## Scripts

| Command | Description |
|---------|-------------|
| `pnpm dev` | Start TypeScript compiler (watch mode) and webpack dev server concurrently |
| `pnpm start` | Start only the webpack dev server |
| `pnpm build` | Production build to `dist/` |
| `pnpm compile` | One-shot TypeScript compilation |
