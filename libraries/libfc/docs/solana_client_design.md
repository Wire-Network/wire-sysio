
# Solana Client Design Document for Claude Code Agent

Features & Capabilities

- Solana JSON RPC Client and supported tools/utils to be created in `fc::network::solana`, files located at `libraries/libfc/include/fc/network/solana/solana_client.hpp`, etc for headers and `libraries/libfc/src/network/solana/solana_client.cpp`, etc for source/implementation files.
- Full support for `ED25519` is supported via `fc::crypto::(public_key|private_key|signature)` with `ED25519` specific storage and implementation is in `fc::crypto::ed`.
- implementation based on the `fc::network::ethereum::ethereum_client`
  - Uses base class `fc::network::json_rpc::json_rpc_client` 
  - Model the use of `fc::variant, fc::variant_object` similarly to the `ethereum_client`
  - Use `signature_provider_manager_plugin` to get `signature_provider` in the exact same way as the `ethereum_client`, with the obvious difference being the `signature_provider` must have a chain type of Solana, which implies its using an `ED25519` key pair
- Create methods for all standard rpc methods, full spec available in `libraries/libfc/docs/solana_client_design.md`
- Program Instruction encode/decode supports both RAW data and Anchor Borsche encoding
  - IDL Parser
  - Borsche encoder/decoder
  - Use the same approach for solana program access as was implemented for etherum contract support; an example is `ethereum_contract_test_counter_client` in `plugins/outpost_ethereum_client_plugin/tools/ethereum_client_rpc_tool/main.cpp`
- Support multiple account keys, on custom programs
- Support all system programs and SYSVARS (Use the typescript repo for guidance if needed, https://github.com/solana-foundation/solana-web3.js)
