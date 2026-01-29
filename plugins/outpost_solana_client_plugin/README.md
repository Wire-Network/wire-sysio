# Outpost Solana Client Plugin

The `outpost_solana_client_plugin` provides Solana JSON-RPC client integration for Wire Sysio. It manages the lifecycle and configuration of one or more `solana_client` instances, each backed by a `signature_provider` for transaction signing.

## Table of Contents

- [Plugin Configuration](#plugin-configuration)
- [Client Architecture](#client-architecture)
- [solana_client (RPC Client)](#solana_client-rpc-client)
- [solana_program_data_client (Raw/Vanilla Programs)](#solana_program_data_client-rawvanilla-programs)
- [solana_program_client (Anchor/IDL Programs)](#solana_program_client-anchoridl-programs)
- [Typed Account Data with FC_REFLECT](#typed-account-data-with-fc_reflect)
- [Supported Borsh Types](#supported-borsh-types)

---

## Plugin Configuration

### Required Plugins

The plugin depends on two other plugins that must also be configured:

- `signature_provider_manager_plugin` -- manages signing keys
- `outpost_client_plugin` -- base outpost client infrastructure

### Command-Line / Config File Options

#### `--signature-provider` (required)

Registers a signing key. Format:

```
<name>,<chain-kind>,<key-type>,<public-key>,<provider-type>:<data>
```

| Field | Description |
|---|---|
| `name` | Lookup ID for this provider (used in `--outpost-solana-client`) |
| `chain-kind` | Target chain: `solana`, `ethereum`, `wire` |
| `key-type` | Key algorithm, e.g. `ed` (ED25519 for Solana) |
| `public-key` | Base58 or hex-encoded public key |
| `provider-type:data` | Either `KEY:<private-key>` or `KIOD:<url>` |

Example using an inline private key:

```
--signature-provider sol-signer,solana,ed,PUB_ED_...,KEY:PVT_ED_...
```

#### `--outpost-solana-client` (required, multi-token)

Registers a Solana RPC client instance. Format:

```
<client-id>,<sig-provider-id>,<rpc-url>
```

| Field | Description |
|---|---|
| `client-id` | Unique identifier for this client instance |
| `sig-provider-id` | Name of a registered `--signature-provider` |
| `rpc-url` | Solana JSON-RPC endpoint URL |

Multiple clients can be configured:

```
--outpost-solana-client devnet-client,sol-signer,https://api.devnet.solana.com
--outpost-solana-client mainnet-client,sol-mainnet-signer,https://api.mainnet-beta.solana.com
```

#### `--solana-idl-file` (optional, multi-token)

Loads one or more Anchor IDL JSON files for use with `solana_program_client`:

```
--solana-idl-file /path/to/counter.json /path/to/another_program.json
```

### Full Example

```bash
./outpost_solana_client_tool \
   --signature-provider sol-signer,solana,ed,PUB_ED_abc123...,KEY:PVT_ED_def456... \
   --outpost-solana-client my-client,sol-signer,https://api.devnet.solana.com \
   --solana-idl-file ./idl/counter_anchor.json
```

### Config File

The same options work in a config `.ini` file:

```ini
signature-provider = sol-signer,solana,ed,PUB_ED_abc123...,KEY:PVT_ED_def456...
outpost-solana-client = my-client,sol-signer,https://api.devnet.solana.com
solana-idl-file = ./idl/counter_anchor.json
```

---

## Client Architecture

```
outpost_solana_client_plugin
   |
   +-- solana_client (JSON-RPC + signing)
          |
          +-- get_program<T>()       --> solana_program_client  (Anchor/IDL)
          +-- get_data_program<T>()  --> solana_program_data_client (raw/vanilla)
```

- **`solana_client`** -- low-level RPC client with transaction signing
- **`solana_program_data_client`** -- base class for raw (non-IDL) program interaction
- **`solana_program_client`** -- base class for Anchor/IDL-based program interaction with automatic account resolution, Borsh encoding/decoding, and typed callable functions

Client instances are cached by `(program_id, client_type)` pair, so calling `get_program<MyClient>(id)` multiple times returns the same instance.

---

## solana_client (RPC Client)

The `solana_client` wraps all standard Solana JSON-RPC methods and provides transaction building/signing.

### Accessing the Client

```cpp
#include <sysio/outpost_solana_client_plugin.hpp>

auto& sol_plug = app->get_plugin<sysio::outpost_solana_client_plugin>();
auto client_entry = sol_plug.get_client("my-client");
auto& client = client_entry->client; // solana_client_ptr
```

### Basic RPC Queries

```cpp
// Chain info
auto slot = client->get_slot();
auto block_height = client->get_block_height();
auto version = client->get_version();

// Account info
auto balance = client->get_balance(client->get_pubkey());
auto account = client->get_account_info(some_address);

// Blockhash
auto blockhash = client->get_latest_blockhash();
```

### Building and Sending Transactions

```cpp
// Build a system transfer instruction
auto instr = system::instructions::transfer(from, to, lamports);

// Create, sign, and send
auto tx = client->create_transaction({instr}, client->get_pubkey());
client->sign_transaction(tx);
auto sig = client->send_and_confirm_transaction(tx);
```

---

## solana_program_data_client (Raw/Vanilla Programs)

For programs without an Anchor IDL. You manually construct instruction data and account lists.

### Defining a Custom Data Client

```cpp
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_system_programs.hpp>

using namespace fc::network::solana;

struct my_counter_client : solana_program_data_client {
   solana_public_key counter_pda;
   uint8_t counter_bump;

   my_counter_client(const solana_client_ptr& client,
                     const solana_public_key& program_id)
      : solana_program_data_client(client, program_id) {
      // Derive the counter PDA
      const char* SEED = "counter";
      std::vector<std::vector<uint8_t>> seeds = {
         std::vector<uint8_t>(SEED, SEED + strlen(SEED))
      };
      std::tie(counter_pda, counter_bump) =
         system::find_program_address(seeds, program_id);
   }

   uint64_t get_counter_value() {
      auto info = client->get_account_info(counter_pda);
      if (!info.has_value() || info->data.size() < 8)
         return 0;
      uint64_t value = 0;
      std::memcpy(&value, info->data.data(), sizeof(uint64_t));
      return value;
   }

   std::string increment(uint64_t amount) {
      // Build raw instruction data (8-byte LE u64)
      std::vector<uint8_t> data(8);
      std::memcpy(data.data(), &amount, sizeof(uint64_t));

      // Build account list
      std::vector<account_meta> accounts = {
         account_meta::signer(client->get_pubkey(), true),
         account_meta::writable(counter_pda, false),
         account_meta::readonly(system::program_ids::SYSTEM_PROGRAM, false)
      };

      return send_and_confirm_tx(data, accounts);
   }
};
```

### Using the Data Client

```cpp
auto program_id = solana_public_key::from_base58("Cdea2BC...");
auto counter = client->get_data_program<my_counter_client>(program_id);

uint64_t value = counter->get_counter_value();
auto sig = counter->increment(1);
```

---

## solana_program_client (Anchor/IDL Programs)

For Anchor programs with IDL definitions. The client automatically handles:

- Borsh encoding/decoding of instruction arguments
- Anchor 8-byte instruction discriminators
- Account resolution (signers, PDAs, fixed addresses)
- PDA derivation from IDL seed definitions
- Account data decoding from IDL type definitions

### Defining an Anchor Client

```cpp
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_system_programs.hpp>

using namespace fc::network::solana;

struct my_anchor_counter : solana_program_client {
   solana_public_key counter_pda;
   uint8_t counter_bump;

   // Typed callable functions -- created from IDL
   solana_program_tx_fn<std::string> initialize;
   solana_program_tx_fn<std::string, uint64_t> increment;
   solana_program_account_data_fn<fc::variant> get_counter;

   my_anchor_counter(const solana_client_ptr& client,
                     const solana_public_key& program_id,
                     const std::vector<idl::program>& idls = {})
      : solana_program_client(client, program_id, idls) {
      // Derive PDA
      const char* SEED = "counter";
      std::vector<std::vector<uint8_t>> seeds = {
         std::vector<uint8_t>(SEED, SEED + strlen(SEED))
      };
      std::tie(counter_pda, counter_bump) =
         system::find_program_address(seeds, program_id);

      // Bind IDL instructions to typed callable functions
      initialize = create_tx<std::string>(get_idl("initialize"));
      increment  = create_tx<std::string, uint64_t>(get_idl("increment"));

      // Bind account data getter (returns fc::variant)
      get_counter = create_account_data_get<fc::variant>("Counter", counter_pda);
   }

   bool is_initialized() {
      auto info = client->get_account_info(counter_pda);
      return info.has_value() && !info->data.empty();
   }

   uint64_t get_counter_value() {
      if (!is_initialized())
         return 0;
      return get_counter(commitment_t::confirmed)["count"].as_uint64();
   }
};
```

### Using the Anchor Client

```cpp
auto program_id = solana_public_key::from_base58("8qR5fPr...");
auto counter = client->get_program<my_anchor_counter>(program_id, all_idls);

// Initialize (accounts auto-resolved from IDL)
if (!counter->is_initialized()) {
   auto sig = counter->initialize();
}

// Read current value
uint64_t value = counter->get_counter_value();

// Increment (argument Borsh-encoded from IDL args definition)
auto sig = counter->increment(5);
```

### Key API

| Method | Description |
|---|---|
| `get_idl(name)` | Look up an IDL instruction definition by name |
| `create_tx<RT, Args...>(instr)` | Create a state-changing transaction function |
| `create_call<RT, Args...>(instr)` | Create a read-only simulation function |
| `create_account_data_get<RT>(type, pda)` | Create a reusable account data getter |
| `get_account_data<RT>(type, address)` | One-shot fetch and decode account data |
| `resolve_accounts(instr, params, overrides)` | Manually resolve accounts from IDL |

---

## Typed Account Data with FC_REFLECT

Instead of accessing decoded account data through `fc::variant` with string key lookups, you can define a C++ struct that mirrors the IDL type and use `FC_REFLECT` for automatic deserialization.

### Example: IDL Type Definition

Given an Anchor IDL with this type:

```json
{
  "types": [
    {
      "name": "Counter",
      "type": {
        "kind": "struct",
        "fields": [
          { "name": "count", "type": "u64" },
          { "name": "bump", "type": "u8" }
        ]
      }
    }
  ]
}
```

### Step 1: Define a Matching C++ Struct

The struct field names **must** match the IDL field names exactly. The IDL decoder first produces an `fc::variant`, and `FC_REFLECT` enables conversion to your struct via `variant.as<T>()`.

```cpp
#include <fc/reflect/reflect.hpp>

struct account_counter_data {
   uint64_t count = 0;
   uint8_t bump = 0;
};
FC_REFLECT(account_counter_data, (count)(bump))
```

### Step 2: Use the Struct as the Template Parameter

```cpp
struct my_typed_counter : solana_program_client {
   solana_public_key counter_pda;
   uint8_t counter_bump;

   solana_program_tx_fn<std::string> initialize;
   solana_program_tx_fn<std::string, uint64_t> increment;

   // Typed getter -- returns account_counter_data directly
   solana_program_account_data_fn<account_counter_data> get_counter;

   my_typed_counter(const solana_client_ptr& client,
                    const solana_public_key& program_id,
                    const std::vector<idl::program>& idls = {})
      : solana_program_client(client, program_id, idls) {
      const char* SEED = "counter";
      std::vector<std::vector<uint8_t>> seeds = {
         std::vector<uint8_t>(SEED, SEED + strlen(SEED))
      };
      std::tie(counter_pda, counter_bump) =
         system::find_program_address(seeds, program_id);

      initialize = create_tx<std::string>(get_idl("initialize"));
      increment  = create_tx<std::string, uint64_t>(get_idl("increment"));

      // Use account_counter_data instead of fc::variant
      get_counter = create_account_data_get<account_counter_data>(
         "Counter", counter_pda);
   }

   uint64_t get_counter_value() {
      if (!is_initialized())
         return 0;
      // Direct struct field access -- no string key lookups
      return get_counter(commitment_t::confirmed).count;
   }

   bool is_initialized() {
      auto info = client->get_account_info(counter_pda);
      return info.has_value() && !info->data.empty();
   }
};
```

### Step 3: Use It

```cpp
auto counter = client->get_program<my_typed_counter>(program_id, all_idls);

// get_counter() returns account_counter_data, not fc::variant
account_counter_data data = counter->get_counter(commitment_t::confirmed);
uint64_t count = data.count;
uint8_t bump = data.bump;
```

### IDL Type to C++ Type Mapping for FC_REFLECT

| IDL Type | C++ Type |
|---|---|
| `bool` | `bool` |
| `u8` | `uint8_t` |
| `u16` | `uint16_t` |
| `u32` | `uint32_t` |
| `u64` | `uint64_t` |
| `u128` | `fc::uint128` |
| `u256` | `fc::uint256` |
| `i8` | `int8_t` |
| `i16` | `int16_t` |
| `i32` | `int32_t` |
| `i64` | `int64_t` |
| `i128` | `fc::int128` |
| `i256` | `fc::int256` |
| `f32` | `float` |
| `f64` | `double` |
| `string` | `std::string` |
| `bytes` | `std::vector<uint8_t>` |
| `publicKey` | `solana_public_key` (use base58 string in variant) |

---

## Supported Borsh Types

The IDL encoder/decoder supports all standard Anchor IDL types:

- **Primitives**: `bool`, `u8`-`u256`, `i8`-`i256`, `f32`, `f64`, `string`, `bytes`, `publicKey`
- **Option**: `{ "option": <type> }` -- encoded as 1-byte tag + value
- **Vec**: `{ "vec": <type> }` -- encoded as 4-byte LE length + elements
- **Array**: `{ "array": [<type>, <size>] }` -- fixed-size, no length prefix
- **Defined (struct/enum)**: `{ "defined": { "name": "<type>" } }` -- resolved from IDL `types` section
- **Struct**: fields encoded in order
- **Enum**: 1-byte variant index + variant fields
