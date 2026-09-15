# signature_provider_manager_plugin

`signature_provider_manager_plugin` owns every signing key the node uses. It parses each
`--signature-provider` spec, builds the matching signer, and publishes the whole set through one lookup
API (by name, by public key, by chain kind, by key type) that `chain_plugin`, `producer_plugin`,
`net_plugin`, the outpost clients, and `underwriter_plugin` query. An operator never enables it by hand:
every one of those plugins names it in `APPBASE_PLUGIN_REQUIRES`, so appbase loads and initializes it
before any consumer. It requires `http_client_plugin`, which it uses for the `KIOD:` scheme.

## How it works

A `--signature-provider` option carries one spec:

```
<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>
```

The four-field form omits `<name>` and the plugin assigns a process-local `key-<n>` instead. `<chain-kind>`
is one of `wire`, `ethereum`, `solana`, `sui`; `<key-type>` is one of `wire`, `wire_bls`, `ethereum`,
`solana`, `sui`. `sui` is not implemented and fails with `pending_impl_exception`; an unrecognised value
fails with `config_parse_error`. `<public-key>` is parsed in the native form of `<key-type>`.

The last field is `<provider-type>:<spec data>`, split on the first colon:

| Scheme | Provided by | What it does |
|---|---|---|
| `KEY:<private-key>` | built in | Parses the private key in the native form of `<key-type>`, asserts the derived public key equals `<public-key>`, and signs locally. |
| `KIOD:<url>` | built in | Signs remotely through a running `kiod`: each signature POSTs `[digest, public-key]` to the URL and reads back a signature. A `unix://` prefix is handed to the HTTP client as a socket path plus request path. |
| `SSM:<param-ref>` | `signature_provider_ssm_plugin` | Fetches the private key once from AWS SSM Parameter Store at startup, then signs locally. |
| `KMS:<key-ref>` | `signature_provider_kms_plugin` | Signs remotely through AWS KMS; the key never leaves AWS. |

`KEY:` and `KIOD:` are handled directly and are not registrable. Every other scheme resolves through a
handler. A provider plugin registers its handler from its **constructor** into a process-wide registry,
which works because appbase constructs all registered plugins before it initializes any; the manager then
honours that scheme only when the providing plugin is enabled with a `plugin =` line. A spec whose scheme
is linked into the binary but whose plugin is not enabled aborts the boot with an error naming the exact
line to add, and a scheme no linked plugin provides aborts with a "no plugin provides this" error. A host
application that embeds the manager without those plugins can instead call `register_spec_handler(...)` on
the instance before `app().initialize(...)`; a handler registered that way is not gated on `--plugin`.

Creation is eager. `plugin_initialize` builds every configured provider, so the full set exists before any
consumer initializes, independent of where the provider plugins' `plugin =` lines sit in the config. A
malformed spec, an unknown or not-enabled scheme, a public key that does not match its private key, or a
duplicate `<name>` / `<public-key>` throws there and the node does not start.

`plugin_startup` then runs the one-shot startup probes. A scheme handler may attach a probe to the provider
it builds — today only `KMS:` does, a free `GetPublicKey` that resolves credentials and checks the pinned
key. A probe that reports a permanent misconfiguration propagates out of `plugin_startup` and aborts the
boot loudly; a transient error is logged and left to the lazy first-sign check, so an AWS blip at restart
cannot block a boot. The probe list is drained, so it runs exactly once. Attaching a probe is the opt-in;
there is no flag to skip it.

The provider set is immutable once the node is running. Every mutator asserts it was called before startup,
and all mutation happens on the main thread during the sequential initialize/startup phases, so runtime
lookups from producer, batch-operator, underwriter, and outpost threads need no synchronization.

### Generated default providers

`chain_plugin` asks the manager for a default `wire` provider when no `wire` provider is configured, and a
default `wire_bls` provider when `producer-name` is set and no `wire_bls` provider is configured. A
generated provider is named `<key-type>-default`, and its `KEY:` spec is persisted to
`<config-dir>/default_signature_providers.json` so the same key survives a restart. That file holds private
keys: it is written through the secure-file helper (owner-only temporary file, fsync, atomic rename), and a
pre-existing copy that is group- or world-readable is brought down to owner-only when it is loaded.

Specs are redacted before they reach a log line or an error message: everything after `KEY:` in the final
field is replaced with `<redacted>`. Only the final field is inspected, so a provider named `KEY:something`
never triggers redaction, and `KIOD:` / `SSM:` / `KMS:` specs pass through unchanged because they carry no
inline secret.

## Enabling / configuration

The plugin is registered by `nodeop` and pulled in automatically by its dependents, so no `plugin =` line
is needed for it. Only the provider plugins are opt-in.

### `config.ini`

```ini
# Named provider, local key.
signature-provider = bp1,wire,wire,<your-wire-public-key>,KEY:<your-wire-private-key>

# Finalizer (BLS) key for a producing node.
signature-provider = bp1-finalizer,wire,wire_bls,<your-bls-public-key>,KEY:<your-bls-private-key>

# Remote signing through a running kiod (unix socket or http URL).
signature-provider = bp1-kiod,wire,wire,<your-wire-public-key>,KIOD:unix:///run/wire/kiod.sock/v1/wallet/sign_digest
signature-provider-kiod-timeout = 5

# AWS-backed schemes require their plugin to be enabled.
plugin = sysio::signature_provider_ssm_plugin
signature-provider = bp1-ssm,wire,wire,<your-wire-public-key>,SSM:us-east-1:/your/ssm/parameter

plugin = sysio::signature_provider_kms_plugin
signature-provider = eth-01,ethereum,ethereum,<your-ethereum-public-key>,KMS:us-east-1:alias/your-signing-key
```

### Command line

```bash
nodeop --signature-provider 'bp1,wire,wire,<your-wire-public-key>,KEY:<your-wire-private-key>' \
       --signature-provider 'bp1-finalizer,wire,wire_bls,<your-bls-public-key>,KEY:<your-bls-private-key>' \
       --signature-provider-kiod-timeout 5
```

A spec passed on the command line is visible in the process listing and in shell history; `config.ini`, a
`KIOD:` provider, or the `SSM:` / `KMS:` schemes avoid that. `nodeop --help` prints both options with their
current defaults, and the `signature-provider` help text enumerates every provider type the binary knows.

## Options

| Option | Default | Meaning |
|---|---|---|
| `signature-provider` | unset | One signing provider, as `<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>`. Repeatable; the four-field form drops `<name>` and receives a process-local `key-<n>`. |
| `signature-provider-kiod-timeout` | `5` | Maximum time in milliseconds allowed for a request to a `KIOD:` provider. A negative value means no deadline. |

Both options are accepted in `config.ini` and on the command line. The plugin registers no other options.

## Diagnostics

The plugin declares no logger of its own, so its output goes to fc's default logger — the one `logging.json`
names `default`.

- **Initialize**, at debug level: `Registering signature provider from spec: <spec>` (redacted) followed by
  `Registered signature provider (<name>): <public-key>` for each spec, and
  `Registering default signature provider spec (type=<key-type>)` when a default is generated.
- **Startup**, at info level: `Running signature-provider startup probes for <n> signing key(s)`, then
  either `Signature-provider startup probes passed` or
  `Signature-provider startup probes passed; <n> key(s) hit a transient error and will be re-checked on the first sign`.
  Nothing is logged when no probe was attached.
- **Startup, transient**, at warning level:
  `Signature-provider startup probe: transient error for one key, deferring its check to the first sign: <detail>`.
- **Key-file permissions**, at warning level: `could not restrict permissions on <file>: <reason>` when the
  defaults file cannot be brought down to owner-only. Loading still succeeds.

Boot-time failures are thrown, not logged, and each names the offending option: an invalid spec (redacted),
a private key that does not match its public key, a duplicate name or public key, an unknown provider type
(the message lists the built-ins and points at the optional plugins), and a scheme whose plugin is not
enabled (the message carries the exact `plugin = ...` line to add).

## Tests

```bash
ninja -C build/debug test_signature_provider_manager_plugin
./build/debug/plugins/signature_provider_manager_plugin/test/test_signature_provider_manager_plugin
```

The suite covers the spec grammar for `wire`, `wire_bls`, `ethereum`, and `solana` keys, the anonymous
four-field form, unknown-scheme errors, instance-registered handlers (including the rejection of built-ins
and duplicates), the startup-probe pass (permanent failure aborts, transient failure defers, probes are
one-shot, a probe is not retained for a rejected duplicate), spec redaction, and the owner-only permissions
on the generated defaults file. The AWS-backed schemes are tested with their own plugins.

## Related plugins

- [`signature_provider_ssm_plugin`](../signature_provider_ssm_plugin/README.md) — supplies the `SSM:` scheme.
- [`signature_provider_kms_plugin`](../signature_provider_kms_plugin/README.md) — supplies the `KMS:` scheme.
- [`wallet_plugin`](../wallet_plugin/README.md) / [`wallet_api_plugin`](../wallet_api_plugin/README.md) —
  the `kiod` side of a `KIOD:` provider.
- `http_client_plugin` — the HTTP client a `KIOD:` provider signs through; required by this plugin.
- `chain_plugin`, `producer_plugin`, `net_plugin`, `outpost_client_plugin`,
  `outpost_ethereum_client_plugin`, `outpost_solana_client_plugin`, `underwriter_plugin` — consumers that
  require this plugin and therefore load it automatically.
- `docs/signature-provider-manager-plugin.md` — the spec reference with worked examples.
