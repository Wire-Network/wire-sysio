# .claude/solana/CLAUDE.md

Session-specific guidance that supplements the root `CLAUDE.md`. This file
captures conventions discovered during active development that are not yet
codified in the root instructions.

## Current Build Directory

The active build directory is **`build/debug-claude`** (not `build/`).
All build and test commands must use this path:

```bash
ninja -C build/debug-claude <target>
./build/debug-claude/libraries/libfc/test/test_fc --run_test=<suite>
```

## C++ Standard & Style

- **C++23** or newer. Always prefer modern idioms (`std::format`, `std::ranges`,
  structured bindings, `std::optional`, `constexpr`, `auto` return types, etc.).
- Use `std::format` style `{}` placeholders everywhere -- never the old
  `${name}` with `("name", value)` macro style.
- 3-space indentation, 120-char line limit (see `.clang-format`).
- Constructor initializers: break before comma.
- Pointer alignment: left (`int* ptr`).
- Run `clang-format -i <file>` on modified files when requested.

## Logging & Assertions

The logging system uses `std::format`-style formatting. The `f`-suffixed and
`_FMT`-suffixed macros have been **removed**. Use the base macros only:

```cpp
// Correct
ilog("Value: {}", value);
dlog("Processing {} items", count);
FC_ASSERT(condition, "Expected {} but got {}", expected, actual);
FC_THROW("Unknown type: {}", type_name);

// Wrong -- these no longer exist
ilogf("Value: {}", value);        // removed
ilog_FMT("Value: {}", value);     // removed
FC_ASSERT(cond, "Bad ${x}", ("x", val));  // old format, do not use
```

## Solana Client Development

### Key Paths

| Component | Path |
|---|---|
| Solana types | `libraries/libfc/include/fc/network/solana/solana_types.hpp` |
| Solana crypto | `libraries/libfc/include/fc/crypto/solana/solana_crypto_utils.hpp` |
| Borsh codec | `libraries/libfc/include/fc/network/solana/solana_borsh.hpp` |
| IDL parser | `libraries/libfc/include/fc/network/solana/solana_idl.hpp` |
| Client header | `libraries/libfc/include/fc/network/solana/solana_client.hpp` |
| Client impl | `libraries/libfc/src/network/solana/solana_client.cpp` |
| System programs | `libraries/libfc/include/fc/network/solana/solana_system_programs.hpp` |
| Tests | `libraries/libfc/test/network/solana/test_solana_client.cpp` |
| Plugin | `plugins/outpost_solana_client_plugin/` |
| Tool | `plugins/outpost_solana_client_plugin/tools/solana_client_rpc_tool/main.cpp` |
| IDL fixture | `tests/fixtures/solana-idl-counter-anchor.json` |

### Build Targets

```bash
ninja -C build/debug-claude fc                            # libfc only
ninja -C build/debug-claude test_fc                       # fc tests
ninja -C build/debug-claude outpost_solana_client_plugin   # plugin lib
ninja -C build/debug-claude outpost_solana_client_tool     # rpc tool
```

### Running Solana Tests

```bash
# All solana tests (currently 47 cases)
./build/debug-claude/libraries/libfc/test/test_fc --run_test=solana_client_tests

# Single test case
./build/debug-claude/libraries/libfc/test/test_fc \
   --run_test=solana_client_tests/test_pubkey_base58_roundtrip
```

### Namespaces & Types

- Solana network types: `fc::network::solana` (transaction, instruction, account_meta, etc.)
- Solana crypto types: `fc::crypto::solana` (solana_public_key, solana_signature, is_on_curve)
- Borsh codec: `fc::network::solana::borsh`
- IDL types: `fc::network::solana::idl`
- System programs: `fc::network::solana::system`

The type `solana_public_key` (in `fc::crypto::solana`) replaced the old `pubkey`
type. Similarly `solana_signature` replaced `signature`. These are re-exported
into `fc::network::solana` via `using` declarations in `solana_types.hpp`.

### Client Hierarchy

```
solana_client                      -- JSON-RPC + signing
   get_program<T>(id, idls)        --> solana_program_client (Anchor/IDL)
   get_data_program<T>(id)         --> solana_program_data_client (raw)
```

- `solana_program_data_client` -- raw instruction data, manual account lists
- `solana_program_client` -- IDL-based with `create_tx`, `create_call`,
  `create_account_data_get`, automatic account resolution, Borsh encode/decode

Program client instances are cached by `std::pair<solana_public_key, demangled_type_name>`.

### Anchor IDL Format

The new Anchor IDL format places account struct fields in the `types` section,
not inline in `accounts`. The `decode_account_data` method handles this by
looking up the type definition from `types` when the account has no inline fields.

### Tool Command Line

```bash
./build/debug-claude/plugins/outpost_solana_client_plugin/outpost_solana_client_tool \
   --signature-provider \
   "sol-01,solana,solana,5dSbb1b9KDpJJpgMuyWDH7HXHn1Aizh7xCY4KXKdSnhH,KEY:4DgyCsQj3ZH519erefdvG8JqzYLky5uyMWcyf4CvygKXfJbegocKBLbhkswL1qETU5ZbHDd7mxuUopjSsq5rWM7R" \
   --outpost-solana-client \
   "sol-local,sol-01,http://localhost:8899" \
   --solana-idl-file \
   tests/fixtures/solana-idl-counter-anchor.json
```

## Ethereum Client (Reference Pattern)

The Solana client mirrors the `ethereum_client` / `ethereum_contract_client`
pattern. When making Solana changes, check the Ethereum counterpart for
consistency:

| Solana | Ethereum |
|---|---|
| `solana_client` | `ethereum_client` |
| `solana_program_client` | `ethereum_contract_client` |
| `solana_program_data_client` | (no direct equivalent) |
| `outpost_solana_client_plugin` | `outpost_ethereum_client_plugin` |

## Common Pitfalls

- **`fc::log_config::initialize_appenders()`** has been removed. Do not call it.
- **`is_on_curve`** checks curve membership only (not subgroup). This is
  intentionally different from libsodium's `crypto_core_ed25519_is_valid_point`.
- **Borsh encoding** is little-endian for all integer types. 128-bit and 256-bit
  integers are encoded as multiple 64-bit LE chunks.
- **`FC_REFLECT` field names** must exactly match IDL field names for typed
  account data deserialization to work.
- The default commitment level across the Solana client API is `confirmed`
  (not `finalized`).
