#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>

namespace sysio {
using namespace fc::network::solana;

/// Program name used in the Anchor IDL for the Solana OPP outpost. Shared
/// between the outpost_solana_client_plugin and the batch_operator_plugin so
/// both speak a single constant when locating the program's IDL entry.
inline constexpr const char* OPP_SOLANA_OUTPOST_PROGRAM_NAME = "opp_outpost";

struct solana_client_entry_t {
   std::string                        id;
   std::string                        url;
   fc::crypto::signature_provider_ptr signature_provider;
   solana_client_ptr                  client;
};

using solana_client_entry_ptr = std::shared_ptr<solana_client_entry_t>;

/// Typed program client for the Solana OPP outpost program. Mirrors the
/// Ethereum `opp_contract_client` / `opp_inbound_contract_client` pattern —
/// each `solana_program_tx_fn<RT, Args...>` is a strongly-typed wrapper that
/// encodes arguments, resolves PDA accounts from the IDL, and submits the tx.
///
/// IDL v2 (Anchor 0.31+) does not embed PDA seeds in the instruction account
/// list, so the static PDAs are pre-derived from the program ID at construction
/// and injected via account_overrides on every call.
///
/// These signatures must stay in sync with `wire-solana/programs/opp-outpost/`
/// (see its generated IDL at `wire-solana/target/idl/opp_outpost.json`).
struct opp_solana_outpost_client : fc::network::solana::solana_program_client {
   // Pre-computed static PDAs (deterministic from program_id).
   fc::network::solana::solana_public_key config_pda;
   fc::network::solana::solana_public_key operator_registry_pda;
   fc::network::solana::solana_public_key message_buffer_pda;

   /// `initialize(consensus_threshold: u32) -> signature`.
   solana_program_tx_fn<std::string, uint32_t>             initialize;
   /// `epoch_in(epoch_index, envelope_data) -> signature`.
   /// epoch_index is used only to derive the per-epoch EpochDeliveries PDA;
   /// the IDL instruction arg is envelope_data only.
   solana_program_tx_fn<std::string, uint32_t, std::vector<uint8_t>> epoch_in;
   /// `emit_outbound_envelope(wire_epoch_index: u32) -> signature`.
   solana_program_tx_fn<std::string, uint32_t>             emit_outbound_envelope;
   /// `add_attestation(attestation_type: i32, data: bytes) -> signature`.
   solana_program_tx_fn<std::string, int32_t, std::vector<uint8_t>> add_attestation;
   /// `deposit(operator_type: u8, wire_account_name: string, amount: u64) -> signature`.
   solana_program_tx_fn<std::string, uint8_t, std::string, uint64_t> deposit;

   opp_solana_outpost_client(const solana_client_ptr& client,
                             const fc::network::solana::solana_public_key& prog_id,
                             const std::vector<fc::network::solana::idl::program>& idls = {})
      : solana_program_client(client, prog_id, idls)
      // Pre-derive static PDAs (seeds match the Rust OUTPOST_CONFIG_SEED etc.)
      , config_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'o','u','t','p','o','s','t','_','c','o','n','f','i','g'}},
           prog_id).first)
      , operator_registry_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'o','p','e','r','a','t','o','r','_','r','e','g','i','s','t','r','y'}},
           prog_id).first)
      , message_buffer_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'m','e','s','s','a','g','e','_','b','u','f','f','e','r'}},
           prog_id).first)
      , initialize(create_tx<std::string, uint32_t>(get_idl("initialize")))
      // epoch_in: epoch_index selects the EpochDeliveries PDA; envelope_data is the IDL arg.
      , epoch_in([this](uint32_t epoch_index, std::vector<uint8_t> env_data) -> std::string {
           // Derive epoch_deliveries PDA: seeds = ["epoch_deliveries", epoch_index_le32]
           std::vector<uint8_t> epoch_seed = {
              static_cast<uint8_t>(epoch_index & 0xFF),
              static_cast<uint8_t>((epoch_index >>  8) & 0xFF),
              static_cast<uint8_t>((epoch_index >> 16) & 0xFF),
              static_cast<uint8_t>((epoch_index >> 24) & 0xFF)
           };
           auto [epoch_deliveries_pda, _b] = fc::network::solana::system::find_program_address(
              {std::vector<uint8_t>{'e','p','o','c','h','_','d','e','l','i','v','e','r','i','e','s'},
               epoch_seed},
              program_id);
           account_overrides_t overrides = {
              {"config",             config_pda},
              {"operator_registry",  operator_registry_pda},
              {"epoch_deliveries",   epoch_deliveries_pda}
           };
           auto& instr = get_idl("epoch_in");
           program_invoke_data_items params = {fc::variant(env_data)};
           return execute_tx(instr, resolve_accounts(instr, params, overrides), params);
        })
      , emit_outbound_envelope([this](uint32_t wire_epoch_index) -> std::string {
           account_overrides_t overrides = {
              {"config",          config_pda},
              {"message_buffer",  message_buffer_pda}
           };
           auto& instr = get_idl("emit_outbound_envelope");
           program_invoke_data_items params = {fc::variant(wire_epoch_index)};
           return execute_tx(instr, resolve_accounts(instr, params, overrides), params);
        })
      , add_attestation(create_tx<std::string, int32_t, std::vector<uint8_t>>(get_idl("add_attestation")))
      , deposit(create_tx<std::string, uint8_t, std::string, uint64_t>(get_idl("deposit"))) {}
};

class outpost_solana_client_plugin : public appbase::plugin<outpost_solana_client_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_client_plugin)(signature_provider_manager_plugin))
   outpost_solana_client_plugin();
   virtual ~outpost_solana_client_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

   std::vector<solana_client_entry_ptr> get_clients();
   solana_client_entry_ptr get_client(const std::string& id);
   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>& get_idl_files();

private:
   std::unique_ptr<class outpost_solana_client_plugin_impl> my;
};

} // namespace sysio
