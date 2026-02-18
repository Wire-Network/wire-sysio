# Implement Outpost Solana Client Plugin

## Plugin Target `outpost_solana_client_plugin`

This plugin is used to interact with the Solana blockchain,
using a command line/config predefined set of target JSON RPC connections.

> **IMPORTANT** This should mimic the `outpost_ethereum_client_plugin`
> located in the `plugins/outpost_ethereum_client_plugin` directory.
> Every possible pattern from the command line argument parsing and
> client configuration should be replicated (where possible, and adjusted as needed).

- The actual `solana_client` is located in `libraries/libfc/src/network/solana/solana_client.cpp` and is part of the
  `fc` target.
- `Claude` generated the `solana_client`, mimicing the `ethereum_client` from
  `libraries/libfc/src/network/ethereum/ethereum_client.cpp`. If implementation changes are needed, then make them as
  long as it stays as close to the `ethereum_client` as possible.
- Target root is `plugins/outpost_solana_client_plugin`
- Stub Declaration is `plugins/outpost_solana_client_plugin/include/sysio/outpost_solana_client_plugin.hpp`
- Stub Implementation is `plugins/outpost_solana_client_plugin/src/outpost_solana_client_plugin.cpp`
- Plugin method `get_clients` should mimic the `get_clients` function on the `outpost_ethereum_client_plugin`
- Plugin method `get_idl_files` should be similar to the `get_abi_files` function on the `outpost_ethereum_client_plugin`

## Tool Target `outpost_solana_client_tool`

- Tool is located at `plugins/outpost_solana_client_plugin/tools/solana_client_rpc_tool/main.cpp`
- Tool should mimic the `outpost_ethereum_client_tool` located in `plugins/outpost_ethereum_client_plugin/tools/ethereum_client_rpc_tool/main.cpp`
- The `solana_program_test_counter_data_client` should be implemented targeting the solana program located at `/data/shared/code/wire/solana-programs/programs/counter/src/lib.rs` and has program id `Cdea2BCiWYBPTQJQq2oWjn5vCkfgENSHNG4GVnWqSvyw`
- The `solana_program_test_counter_anchor_client` should be implemented targeting the solana program located at `/data/shared/code/wire/solana-programs/programs/counter_anchor/src/lib.rs` and has program id `8qR5fPrG9YWSWc68NLArP8m4JhM4e1T3aJ4waV9RKYQb`

### Example command line for tool

> **NOTE** The following command line only works when cwd is the root of the project

```shell
./build/debug-clang-clion/plugins/outpost_solana_client_plugin/outpost_solana_client_tool \
  --signature-provider \
  "wire-01,wire,wire,SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm,KEY:5J5LzjfChtY3LGhkxaRoAaSjKHtgNZqKyaJaw5boxuY9LNv4e1U" \
  --signature-provider \
  "eth-01,ethereum,ethereum,0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80" \
  --signature-provider \
  "sol-01,solana,solana,5dSbb1b9KDpJJpgMuyWDH7HXHn1Aizh7xCY4KXKdSnhH,KEY:4DgyCsQj3ZH519erefdvG8JqzYLky5uyMWcyf4CvygKXfJbegocKBLbhkswL1qETU5ZbHDd7mxuUopjSsq5rWM7R" \
  --outpost-solana-client \
  "sol-local,sol-01,http://localhost:8899" \
  --solana-idl-file \
  tests/fixtures/solana-idl-counter-anchor.json
```
#### Args for clion run configuration
```sh
--signature-provider
"wire-01,wire,wire,SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm,KEY:5J5LzjfChtY3LGhkxaRoAaSjKHtgNZqKyaJaw5boxuY9LNv4e1U"
--signature-provider
"eth-01,ethereum,ethereum,0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
--signature-provider
"sol-01,solana,solana,5dSbb1b9KDpJJpgMuyWDH7HXHn1Aizh7xCY4KXKdSnhH,KEY:4DgyCsQj3ZH519erefdvG8JqzYLky5uyMWcyf4CvygKXfJbegocKBLbhkswL1qETU5ZbHDd7mxuUopjSsq5rWM7R"
--outpost-solana-client
"sol-local,sol-01,http://localhost:8899"
--solana-idl-file
tests/fixtures/solana-idl-counter-anchor.json
```
