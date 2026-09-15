# wallet_plugin

`wallet_plugin` is the key store behind `kiod`. It owns a directory of encrypted wallet files, tracks which
of them are unlocked, and performs signing on behalf of callers that hold a public key but not the matching
private key. It exposes no HTTP surface of its own — [`wallet_api_plugin`](../wallet_api_plugin/README.md)
does that — and it declares no plugin dependencies. `kiod` starts it unconditionally (its `main` initializes
`wallet_plugin`, `wallet_api_plugin`, and `http_plugin` directly), so there is no `plugin =` line to add;
only its options are operator-facing. `nodeop` does not load it.

## How it works

The plugin creates a single `wallet_manager` during `plugin_initialize` and hands a reference to it to
`wallet_api_plugin`. `plugin_startup` and `plugin_shutdown` are empty, and the plugin starts no thread pool
of its own.

### The wallet directory

`wallet-dir` names the directory; a relative value resolves against the application data directory (for
`kiod`, `~/.config/wire/kiod/data` unless `--data-dir` says otherwise), and the directory is created if it
does not exist. Each wallet is one file, `<name>.wallet`, holding a JSON object with a `version` and a
`cipher_keys` byte array. A wallet name must be a single path component made only of alphanumerics, `.`,
`_`, and `-`; anything else is rejected, so a name can never escape the directory.

Setting the directory also takes an exclusive lock on it. The manager creates `wallet.lock` inside the
directory and holds an interprocess file lock on it; if the lock cannot be taken, startup fails with
"Failed to lock access to wallet directory; is another kiod running?". A one-second timer then watches the
lock file, and if it disappears while the process is running, the application quits — a deleted lock file
means another process could claim the directory. The lock file is removed when the manager is destroyed.

### Encryption and locking

Creating a wallet generates its password: the literal prefix `PW` followed by a freshly generated private
key in string form. There is no user-supplied password — the caller must record what `create` returns or
lose access to the wallet. The password's SHA-512 hash is the wallet's checksum and its AES key; the key
list is packed (with the checksum embedded) and AES-encrypted into `cipher_keys`, and the file is written
with the process umask temporarily tightened so group and other get no bits. Unlocking decrypts
`cipher_keys` with the hash of the supplied password and checks the embedded checksum, so a wrong password
fails cleanly. Locking discards the plaintext keys and the checksum from memory.

Nearly every wallet-manager call begins with an inactivity check: if `unlock-timeout` seconds have elapsed
since the previous call, all wallets are locked before the current call proceeds; otherwise the deadline is
pushed forward. A read-only call such as listing wallets counts as activity just as a signature does. The two
exceptions are locking everything, which skips the check because it is locking everything anyway, and
replacing the timeout, which restarts the countdown directly. `unlock-timeout` must be positive — a zero
or negative value fails initialization with `invalid_lock_timeout_exception`.

### Keys

A wallet holds key pairs indexed by public key and, in parallel, by an operator-facing name. Importing a key
without a name assigns a generated one (`my-key-0`, `my-key-1`, …, first unused). `create_key` generates a
new pair inside an unlocked wallet; the key type is upper-cased before dispatch and must be one of:

| Key type | Curve |
|---|---|
| `K1` | secp256k1 (the default when the type is empty) |
| `R1` | secp256r1 |
| `EM` | ethereum secp256k1 |
| `ED` | ed25519 |

Anything else raises `unsupported_key_type_exception`. Importing a key, generating one, and removing one each
write the wallet file immediately. The `set_key_name` operations update only the in-memory index, so a rename
reaches disk with the next operation that writes the file.

Signing walks the unlocked wallets. `sign_transaction` takes a transaction, the set of public keys that must
sign it, and a chain id, and appends one signature per key — failing if any key is not in an unlocked
wallet. `sign_digest` signs a bare digest with one named public key, which is the shape a `KIOD:` signature
provider in `nodeop` calls. Operations that reveal or remove key material (`list_keys`, `list_keys_by_name`,
`remove_key`, `remove_name`, the `set_key_name` variants) require the wallet to be both unlocked *and* the
correct password supplied again on the call.

## Enabling / configuration

The plugin runs inside `kiod`, which initializes it directly — there is no `plugin =` line for it.

### `config.ini` (in `kiod`'s config directory, by default `~/.config/wire/kiod/config`)

```ini
# Relative to the data dir; created if missing.
wallet-dir = .

# Lock every wallet after 15 minutes without a wallet call.
unlock-timeout = 900
```

### Command line

```bash
kiod --wallet-dir /var/lib/wire/wallets \
     --unlock-timeout 900
```

Both options are accepted in `config.ini` and on the command line; `kiod --help` prints them with their
current defaults.

## Options

| Option | Default | Meaning |
|---|---|---|
| `wallet-dir` | `.` | Directory holding the `*.wallet` files and `wallet.lock`. An absolute path is used as given; a relative path resolves against the application data directory. Created if it does not exist. |
| `unlock-timeout` | `900` | Seconds of inactivity after which every unlocked wallet is locked. Any wallet-manager call counts as activity. Must be greater than zero. |

The plugin registers no other options.

## Diagnostics

The plugin declares no logger of its own, so its output goes to fc's default logger — the one `logging.json`
names `default`.

- `initializing wallet plugin` at info level, once, at the start of `plugin_initialize`.
- `saving wallet to file <path>` at warning level, every time a wallet file is written — which is after every
  create, import, key removal, rename, and password change, not only on shutdown.
- `Unable to open file: <path>` at error level when a wallet file cannot be written, immediately before the
  matching exception.

Everything else surfaces as a thrown exception with a message naming the wallet: a wallet that already
exists, a wallet file that cannot be opened, a locked wallet, an already-unlocked wallet, a bad password, a
missing public key when signing, an unsupported key type, a name containing a path separator, a
non-positive `unlock-timeout`, and the directory-lock failure described above. `kiod` reports these to the
caller through `wallet_api_plugin`'s HTTP error responses.

## Tests

The plugin has no `test/` directory of its own; it is covered by the `plugin_test` binary, whose
`wallet_tests` suite exercises the soft wallet and the wallet manager.

```bash
ninja -C build/debug plugin_test
./build/debug/tests/plugin_test --run_test=wallet_tests
```

## Related plugins

- [`wallet_api_plugin`](../wallet_api_plugin/README.md) — publishes this plugin's operations as
  `/v1/wallet/*` HTTP endpoints; the two are started together by `kiod`.
- [`signature_provider_manager_plugin`](../signature_provider_manager_plugin/README.md) — its `KIOD:<url>`
  scheme is how `nodeop` signs through a running `kiod`.
- `http_plugin` — serves the wallet endpoints; `kiod` configures it to listen on a unix socket by default.
