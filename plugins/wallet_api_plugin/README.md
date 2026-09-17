# wallet_api_plugin

`wallet_api_plugin` publishes [`wallet_plugin`](../wallet_plugin/README.md)'s key store as a set of
`/v1/wallet/*` HTTP endpoints. It is the API surface `clio` talks to when it needs a signature, and the
endpoint a `nodeop` `KIOD:` signature provider posts digests to. It requires `wallet_plugin` and
`http_plugin`, and `kiod` starts all three directly, so there is no `plugin =` line to add — the plugin is
present whenever `kiod` is running. `nodeop` does not load it.

## How it works

`plugin_initialize` does one thing: it asks `http_plugin` whether the `node` API category is reachable only
over loopback, and if it is not, logs a boxed **SECURITY ERROR** banner warning that the wallet API is
exposed to the local network, that the HTTP RPC is unencrypted, and that passwords and private keys are at
high risk. It does not refuse to start.

`plugin_startup` takes a reference to the running `wallet_manager` and registers every handler below in the
`node` API category on the read-write execution queue. Each handler parses its JSON body, calls straight
through to the wallet manager, and serializes the result; any exception is turned into the standard HTTP
error response, so a locked wallet, a wrong password, or a missing key comes back as a structured error
rather than a dropped connection. `plugin_shutdown` is empty, and the plugin holds no state of its own —
the wallet manager's inactivity timer means an idle window can lock every wallet between two calls.

`kiod` configures `http_plugin` with a default unix socket (`kiod.sock`, relative to the data directory, so
`~/.config/wire/kiod/data/kiod.sock` by default), no default TCP address, and per-category addresses
disabled. Left alone, the wallet API is therefore reachable only through that socket — which is also
`clio`'s default wallet URL. Setting `http-server-address` turns on a TCP listener for the same endpoints;
binding it anywhere other than loopback is what triggers the banner above.

## Enabling / configuration

The plugin registers no options and needs no `plugin =` line. What an operator configures is the transport,
through `http_plugin`:

### `config.ini` (in `kiod`'s config directory, by default `~/.config/wire/kiod/config`)

```ini
# The default: a unix socket under the data dir, no TCP listener.
unix-socket-path = kiod.sock

# Optional TCP listener. Keep it on loopback: the wallet API is unauthenticated and unencrypted.
http-server-address = 127.0.0.1:8900
```

### Command line

```bash
kiod --data-dir /var/lib/wire \
     --unix-socket-path kiod.sock \
     --wallet-dir /var/lib/wire/wallets \
     --unlock-timeout 900
```

`--data-dir` is what moves the socket. A relative `--unix-socket-path` is resolved against the data dir by
`http_plugin`, and `--wallet-dir` has no bearing on it — so without the `--data-dir` line above the socket
would still be created at `~/.config/wire/kiod/data/kiod.sock`. Passing an absolute
`--unix-socket-path /var/lib/wire/kiod.sock` has the same effect without relocating the rest of the data dir.

Point a client at whichever path the socket actually lives on: `clio --wallet-url
unix:///var/lib/wire/kiod.sock ...` matches the invocation above, and `WALLET_URL` sets the same thing.
`clio`'s built-in default is `unix://$HOME/.config/wire/kiod/data/kiod.sock`.

## Options

The plugin registers no program options — its `set_program_options` is empty. Wallet behavior comes from
`wallet_plugin` (`wallet-dir`, `unlock-timeout`) and the transport from `http_plugin`.

## HTTP API

Every endpoint is `POST`, in the `node` API category, with a JSON request body. A call taking more than one
value takes them as a **positional JSON array**, in the order given below, and rejects an array of the wrong
length. A call taking exactly one value takes that value bare (`"my-wallet"`, `900`). A call taking none
requires an empty body and rejects a non-empty one. Endpoints that return nothing respond with `{}`.

| Endpoint | Body | Status | Purpose |
|---|---|---|---|
| `/v1/wallet/create` | `"<wallet-name>"` | 201 | Create `<wallet-name>.wallet`, leave it unlocked, and return the generated password. Nothing else ever reveals it. |
| `/v1/wallet/open` | `"<wallet-name>"` | 200 | Load an existing wallet file into the manager. Does not unlock it. |
| `/v1/wallet/list_wallets` | *(empty)* | 200 | Every loaded wallet's name, with ` *` appended when it is unlocked. |
| `/v1/wallet/unlock` | `["<wallet-name>", "<password>"]` | 200 | Unlock a wallet, opening it first if it is not loaded. Fails if it is already unlocked. |
| `/v1/wallet/lock` | `"<wallet-name>"` | 200 | Lock one wallet. A no-op if it is already locked. |
| `/v1/wallet/lock_all` | *(empty)* | 200 | Lock every unlocked wallet. |
| `/v1/wallet/set_timeout` | `<seconds>` | 200 | Replace the inactivity timeout and restart the countdown. |
| `/v1/wallet/import_key` | `["<wallet-name>", "<private-key>"]` | 201 | Import a private key into an unlocked wallet under a generated name. |
| `/v1/wallet/create_key` | `["<wallet-name>", "<key-type>"]` | 201 | Generate a key inside an unlocked wallet and return its public key. Key type is `K1`, `R1`, `EM`, or `ED`; empty means `K1`. |
| `/v1/wallet/remove_key` | `["<wallet-name>", "<password>", "<public-key>"]` | 201 | Remove the key with that public key. Requires the wallet unlocked and the password re-supplied. |
| `/v1/wallet/remove_name` | `["<wallet-name>", "<password>", "<key-name>"]` | 201 | Remove the key carrying that name. Requires the wallet unlocked and the password re-supplied. |
| `/v1/wallet/list_keys` | `["<wallet-name>", "<password>"]` | 200 | Every key pair in that wallet, as public key to private key. Requires the wallet unlocked and the password re-supplied. |
| `/v1/wallet/list_keys_by_name` | `["<wallet-name>", "<password>"]` | 200 | The same key material indexed by key name, each entry carrying name, public key, and private key. Same requirements. |
| `/v1/wallet/get_public_keys` | *(empty)* | 200 | Every public key across all unlocked wallets. Fails when no wallet is loaded, or when every loaded wallet is locked. |
| `/v1/wallet/set_key_name_with_public_key` | `["<wallet-name>", "<password>", "<key-name>", "<public-key>"]` | 200 | Name the key identified by that public key. |
| `/v1/wallet/set_key_name_with_private_key` | `["<wallet-name>", "<password>", "<key-name>", "<private-key>"]` | 200 | Name the key identified by that private key. |
| `/v1/wallet/set_key_name` | `["<wallet-name>", "<password>", "<new-key-name>", "<current-key-name>"]` | 200 | Rename an already-named key. |
| `/v1/wallet/sign_transaction` | `[<transaction>, [<public-key>, ...], "<chain-id>"]` | 201 | Append one signature per listed public key and return the signed transaction. Fails if any key is not in an unlocked wallet. |
| `/v1/wallet/sign_digest` | `["<digest>", "<public-key>"]` | 201 | Sign a bare 32-byte digest with one key from an unlocked wallet. This is the endpoint a `KIOD:` signature provider calls. |

Every endpoint except `lock_all` and `set_timeout` runs the wallet manager's inactivity check first, so a call
arriving after `unlock-timeout` has elapsed locks every wallet before it runs, and a call arriving sooner
pushes the deadline forward. `kiod` itself adds one further endpoint outside this plugin, `/v1/kiod/stop`,
which shuts the process down.

## Diagnostics

The plugin declares no logger of its own, so its output goes to fc's default logger — the one `logging.json`
names `default`. It logs exactly two things:

- The boxed `********!!!SECURITY ERROR!!!********` banner at error level during initialization, when the
  `node` API category is bound to anything other than loopback. Treat it as a configuration to fix, not as
  noise: the wallet API has no authentication and the transport is not encrypted.
- `starting wallet_api_plugin` at debug level, immediately before the handlers are registered.

Per-request failures are not logged here; they are returned to the caller as HTTP errors carrying the
wallet-layer message (wallet not found, wallet is locked, invalid password, public key not found in
unlocked wallets, unsupported key type, and so on). A malformed body comes back as an invalid-request error
— a missing body where one is required, a body where none is allowed, or an array of the wrong length.

## Tests

The plugin has no `test/` directory of its own. Its wallet behavior is covered through `wallet_plugin` by
the `plugin_test` binary's `wallet_tests` suite.

```bash
ninja -C build/debug plugin_test
./build/debug/tests/plugin_test --run_test=wallet_tests
```

## Related plugins

- [`wallet_plugin`](../wallet_plugin/README.md) — required; owns the wallet files, the locking, and every
  operation these endpoints expose.
- `http_plugin` — required; serves the endpoints and decides where they are reachable.
- [`signature_provider_manager_plugin`](../signature_provider_manager_plugin/README.md) — its `KIOD:<url>`
  scheme points `nodeop` at `/v1/wallet/sign_digest` here.
