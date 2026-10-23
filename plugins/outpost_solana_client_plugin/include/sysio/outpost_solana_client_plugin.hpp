#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>

namespace sysio {
using namespace fc::network::solana;

/// Default IDL name; integrated liqsol-core deployments override it through configuration.
inline constexpr const char* OPP_SOLANA_OUTPOST_PROGRAM_NAME = "opp_outpost";

/// Heap frame requested by the zero-data terminal `epoch_in` call, in bytes.
///
/// Data chunks stay bare so a full chunk remains inside Solana's packet limit.
/// The terminal call decodes the staged envelope and records consensus, so it
/// uses the same 256,000-byte frame exercised by the producer capacity suite.
inline constexpr uint32_t SOLANA_TERMINAL_EPOCH_IN_HEAP_FRAME_BYTES = 256'000;

/// Compute-unit ceiling requested by the zero-data terminal `epoch_in` call.
///
/// This is Solana's per-transaction ceiling and matches the producer's
/// full-envelope capacity tests. Requesting it explicitly prevents the
/// terminal path from silently falling back to the approximately 200,000-CU
/// default as the envelope and roster approach their supported bounds.
inline constexpr uint32_t SOLANA_TERMINAL_EPOCH_IN_COMPUTE_UNIT_LIMIT = 1'400'000;

namespace outpost_solana_client_detail {

/// Build the exact ComputeBudget preamble for one production `epoch_in` call.
///
/// Non-terminal data chunks intentionally return an empty preamble. The
/// zero-data terminal call requests the producer-tested heap frame followed by
/// the producer-tested compute-unit ceiling. Keeping this composition in one
/// helper lets packet tests exercise the same instruction vector the relay
/// submits.
inline std::vector<fc::network::solana::instruction> epoch_in_pre_instructions(uint16_t chunk_index,
                                                                               uint16_t total_chunks) {
   if (chunk_index != total_chunks)
      return {};
   return {
      fc::network::solana::system::compute_budget::request_heap_frame(SOLANA_TERMINAL_EPOCH_IN_HEAP_FRAME_BYTES),
      fc::network::solana::system::compute_budget::set_compute_unit_limit(SOLANA_TERMINAL_EPOCH_IN_COMPUTE_UNIT_LIMIT),
   };
}

} // namespace outpost_solana_client_detail

/// Consumer role a Solana `outpost_client` is constructed for. Boot-time IDL
/// validation is role-aware: only roles that call `read_inbound_envelope`
/// require the loaded IDL to declare a decodable `LatestOutboundEnvelope`
/// account, so an instructions-only IDL keeps working for roles that never
/// read inbound. A new role must pick the variant matching whether it reads
/// inbound envelopes.
enum class solana_outpost_role {
   /// Delivers outbound envelopes AND polls `read_inbound_envelope` - the
   /// IDL must declare a readable `LatestOutboundEnvelope`, asserted at boot.
   batch_operator,
   /// Reads only the outpost's record of the envelopes it emitted
   /// (`read_emitted_envelope_digest`) - the IDL must declare a readable
   /// outbound `EnvelopeLog`, asserted at boot.
   underwriter,

};

struct solana_client_entry_t {
   std::string                        id;
   std::string                        url;
   fc::crypto::signature_provider_ptr signature_provider;
   solana_client_ptr                  client;
};

using solana_client_entry_ptr = std::shared_ptr<solana_client_entry_t>;

/**
 * @brief Filter loaded IDL definitions down to the OPP outpost program.
 *
 * Walks `idl_files` (shaped like `outpost_solana_client_plugin::get_idl_files()`)
 * and returns every program definition whose IDL name equals `program_name`, so
 * an outpost client is never constructed around an unrelated IDL.
 *
 * @param idl_files    Loaded IDL files: one `(path, programs)` pair per file.
 * @param program_name IDL program name to match — the configured
 *                     `--solana-outpost-program-name` (default
 *                     `OPP_SOLANA_OUTPOST_PROGRAM_NAME`).
 * @return Matching program definitions (empty when none match).
 */
std::vector<fc::network::solana::idl::program> filter_outpost_program_idls(
   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>& idl_files,
   std::string_view program_name);

/// Typed program client for the Solana OPP outpost program. Mirrors the
/// Ethereum `opp_contract_client` / `opp_inbound_contract_client` pattern —
/// each `solana_program_tx_fn<RT, Args...>` is a strongly-typed wrapper that
/// encodes arguments, resolves PDA accounts from the IDL, and submits the tx.
///
/// IDL v2 (Anchor 0.31+) does not embed PDA seeds in the instruction account
/// list, so the static PDAs are pre-derived from the program ID at construction
/// and injected via account_overrides on every call.
///
/// These signatures must stay in sync with the Solana OPP outpost program —
/// since the clean-room rewrite it is hosted inside
/// `wire-solana/programs/liqsol-core/` (`src/instructions/opp/`; generated IDL
/// at `wire-solana/target/idl/liqsol_core.json`).
struct opp_solana_outpost_client : fc::network::solana::solana_program_client {
   // Pre-computed static PDAs (deterministic from program_id).
   fc::network::solana::solana_public_key config_pda;
   fc::network::solana::solana_public_key operator_registry_pda;
   fc::network::solana::solana_public_key outbound_message_buffer_pda;
   /// Singleton inbound-envelope log (WIRE → SOL records). All writes and
   /// reads go through this one PDA; pruning is internal to the Vec so the
   /// client never sees per-epoch accounts.
   fc::network::solana::solana_public_key inbound_envelopes_pda;
   /// Singleton outbound-envelope log (SOL → WIRE records). Same shape.
   fc::network::solana::solana_public_key outbound_envelopes_pda;
   /// Single-slot PDA holding the most recent outbound envelope's raw
   /// bytes — overwritten on every emit. The WIRE batch operator reads
   /// this to relay the envelope back to WIRE.
   fc::network::solana::solana_public_key latest_outbound_envelope_pda;
   /// `initialize(consensus_threshold: u32) -> signature`.
   solana_program_tx_fn<std::string, uint32_t>             initialize;
   /// `epoch_in(epoch_index, chunk_index, total_chunks, total_bytes, chunk_data,
   ///           extra_remaining_accounts) -> signature`.
   ///
   /// Inbound delivery is chunked: Solana's 1 232-byte tx MTU can't carry
   /// a full OPP envelope at production roster sizes, so the caller streams
   /// the envelope into a per-(epoch, signer) staging PDA and the program
   /// finalizes only on a zero-data terminal call where
   /// `chunk_index == total_chunks`. `epoch_index` selects both the per-epoch
   /// EpochDeliveries PDA and the per-(epoch, signer) chunk-buffer PDA.
   ///
   /// `extra_remaining_accounts` is appended past the IDL's account list as
   /// Anchor `remaining_accounts`. Production delivery passes an empty slice
   /// for both data chunks and the zero-data terminal call: that call only
   /// finalizes the envelope and records consensus. Effect accounts for LIQ
   /// custody, recipient wallets, canonical ATAs, mints, and token programs
   /// travel on the separate `dispatch_attestations` crank.
   solana_program_tx_fn<std::string, uint32_t, uint16_t, uint16_t, uint32_t,
                         std::vector<uint8_t>,
                         std::vector<fc::network::solana::account_meta>> epoch_in;
   /// `cleanup_envelope_chunks(epoch_index) -> signature`.
   /// Permissionless reaper for chunk buffers an operator started but
   /// never finished. Callable once the chain has advanced past
   /// `epoch_index`. Rent returns to the original uploader.
   solana_program_tx_fn<std::string, uint32_t>             cleanup_envelope_chunks;
   /// `emit_outbound_envelope(wire_epoch_index: u32) -> signature`.
   /// Recovery/admin escape hatch only. The steady-state batch-operator relay
   /// never calls it because the draining `dispatch_attestations` round emits
   /// the outbound envelope inline.
   solana_program_tx_fn<std::string, uint32_t>             emit_outbound_envelope;
   /// Decode already-fetched Anchor account bytes using the outpost IDL.
   /// This lets callers distinguish account-not-found from RPC/transport
   /// failure before decoding the returned data.
   fc::variant decode_account_info_data(const std::string& account_name,
                                        const std::vector<uint8_t>& data) {
      return decode_account_data(data, account_name);
   }

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
      , outbound_message_buffer_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'o','u','t','b','o','u','n','d','_','m','e','s','s','a','g','e','_','b','u','f','f','e','r'}},
           prog_id).first)
      , inbound_envelopes_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'i','n','b','o','u','n','d','_','e','n','v','e','l','o','p','e','s'}},
           prog_id).first)
      , outbound_envelopes_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'o','u','t','b','o','u','n','d','_','e','n','v','e','l','o','p','e','s'}},
           prog_id).first)
      , latest_outbound_envelope_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'l','a','t','e','s','t','_','o','u','t','b','o','u','n','d','_','e','n','v','e','l','o','p','e'}},
           prog_id).first)
      // OPP writes default to the confirmed variant — any state-changing
      // call on this client is consensus-critical and must not silently
      // drop (see epoch-859 stall RCA). `execute_tx_and_confirm` + default
      // `solana_confirm_options` (commitment=processed, 15s budget) gives
      // fast failure signal while still proving on-chain acceptance.
      , initialize(create_tx_and_confirm<std::string, uint32_t>(get_idl("initialize")))
      // epoch_in: chunked. epoch_index drives both the EpochDeliveries PDA
      // and the per-(epoch, signer) chunk-buffer PDA; the chunked args are
      // forwarded as the IDL params.
      , epoch_in([this](uint32_t epoch_index,
                        uint16_t chunk_index,
                        uint16_t total_chunks,
                        uint32_t total_bytes,
                        std::vector<uint8_t> chunk_data,
                        std::vector<fc::network::solana::account_meta> extra_remaining_accounts) -> std::string {
           const std::vector<uint8_t> epoch_seed = {
              static_cast<uint8_t>(epoch_index & 0xFF),
              static_cast<uint8_t>((epoch_index >>  8) & 0xFF),
              static_cast<uint8_t>((epoch_index >> 16) & 0xFF),
              static_cast<uint8_t>((epoch_index >> 24) & 0xFF)
           };
           auto [epoch_deliveries_pda, _epoch_bump] =
              fc::network::solana::system::find_program_address(
                 {std::vector<uint8_t>{'e','p','o','c','h','_','d','e','l','i','v','e','r','i','e','s'},
                  epoch_seed},
                 program_id);
           // Per-(epoch, signer) chunk buffer. The signer's pubkey IS the
           // third seed — multiple operators in the same group write to
           // their own buffers without contention.
           const auto signer_pk = this->client->get_pubkey().serialize();
           auto [chunk_buffer_pda, _chunk_bump] =
              fc::network::solana::system::find_program_address(
                 {std::vector<uint8_t>{'e','n','v','e','l','o','p','e','_','c','h','u','n','k','s'},
                  epoch_seed,
                  std::vector<uint8_t>(signer_pk.begin(), signer_pk.end())},
                 program_id);
           // `epoch_in` only stages/finalizes inbound chunks and records the
           // delivery/consensus result. It neither applies the enclosed
           // attestations nor fires an outbound emit; both belong to the
           // `dispatch_attestations` crank below, which drains the epoch's
           // attestation cursor and carries the effect/outbound accounts.
           // The IDL's `epoch_in` account list is just the 7 delivery/
           // staging/consensus accounts below; no outbound PDAs needed here.
           account_overrides_t overrides = {
              {"config",                    config_pda},
              {"operator_registry",         operator_registry_pda},
              {"epoch_deliveries",          epoch_deliveries_pda},
              {"chunk_buffer",              chunk_buffer_pda},
              {"inbound_envelopes",         inbound_envelopes_pda},
           };
           auto& instr = get_idl("epoch_in");
           program_invoke_data_items params = {
              fc::variant(epoch_index),
              fc::variant(chunk_index),
              fc::variant(total_chunks),
              fc::variant(total_bytes),
              fc::variant(chunk_data),
           };

           // ComputeBudget pre-ixs are injected ONLY on the zero-data
           // terminal call. That call decodes the staged envelope, hashes it,
           // records the delivery, and runs consensus.
           // The producer's cap-sized capacity suite exercises that path with
           // a 256,000-byte heap and Solana's 1,400,000-CU transaction ceiling.
           //
           // Data chunks only copy their bounded slice into the zero-copy
           // staging account. They need no budget bump, and keeping them bare
           // preserves the full-chunk packet-size invariant.
           auto pre_ixs = outpost_solana_client_detail::epoch_in_pre_instructions(chunk_index, total_chunks);
           // Resolve the IDL's declared accounts first, then append any
           // caller-supplied `remaining_accounts`. Production delivery uses
           // none; settlement effect accounts belong to
           // `dispatch_attestations`.
           // Anchor's runtime exposes everything past the IDL's declared
           // accounts as `ctx.remaining_accounts`; the relay supplies full
           // account metas so writable/readonly flags match each effect
           // handler's requirements.
           auto accounts = resolve_accounts(instr, params, overrides);
           accounts.reserve(accounts.size() + extra_remaining_accounts.size());
           accounts.insert(accounts.end(), extra_remaining_accounts.begin(), extra_remaining_accounts.end());
           return execute_tx_and_confirm(instr, accounts, params, pre_ixs);
        })
      , cleanup_envelope_chunks([this](uint32_t epoch_index) -> std::string {
           const std::vector<uint8_t> epoch_seed = {
              static_cast<uint8_t>(epoch_index & 0xFF),
              static_cast<uint8_t>((epoch_index >>  8) & 0xFF),
              static_cast<uint8_t>((epoch_index >> 16) & 0xFF),
              static_cast<uint8_t>((epoch_index >> 24) & 0xFF)
           };
           // The reaper closes ITS OWN buffer (caller's pubkey is the
           // third seed). For closing someone else's stale buffer, rebuild
           // a sibling lambda that takes an explicit uploader pubkey.
           const auto signer_pk = this->client->get_pubkey().serialize();
           auto [chunk_buffer_pda, _bump] =
              fc::network::solana::system::find_program_address(
                 {std::vector<uint8_t>{'e','n','v','e','l','o','p','e','_','c','h','u','n','k','s'},
                  epoch_seed,
                  std::vector<uint8_t>(signer_pk.begin(), signer_pk.end())},
                 program_id);
           account_overrides_t overrides = {
              {"config",                    config_pda},
              {"latest_outbound_envelope",  latest_outbound_envelope_pda},
              {"chunk_buffer",              chunk_buffer_pda},
              // `uploader` resolves to the signer (caller closes their own
              // buffer); the IDL marks it `mut` and Anchor's seeds-derived
              // close target accepts any pubkey that matches the seeds.
           };
           auto& instr = get_idl("cleanup_envelope_chunks");
           program_invoke_data_items params = {fc::variant(epoch_index)};
           return execute_tx_and_confirm(instr, resolve_accounts(instr, params, overrides), params);
        })
      // Retained as the program's explicit recovery/admin escape hatch. The
      // steady-state relay intentionally uses only terminal `epoch_in`.
      , emit_outbound_envelope([this](uint32_t wire_epoch_index) -> std::string {
           account_overrides_t overrides = {
              {"config",                    config_pda},
              {"outbound_message_buffer",   outbound_message_buffer_pda},
              {"outbound_envelopes",        outbound_envelopes_pda},
              {"latest_outbound_envelope",  latest_outbound_envelope_pda},
           };
           auto& instr = get_idl("emit_outbound_envelope");
           program_invoke_data_items params = {fc::variant(wire_epoch_index)};
           return execute_tx_and_confirm(instr, resolve_accounts(instr, params, overrides), params);
        })
 {}
};

class outpost_solana_client_plugin : public appbase::plugin<outpost_solana_client_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_client_plugin)(signature_provider_manager_plugin))
   outpost_solana_client_plugin();
   virtual ~outpost_solana_client_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

   std::vector<solana_client_entry_ptr> get_clients();
   /// Return the configured client registered under `id`, or nullptr when there
   /// is none. For an outpost the id is the chain's `sysio.chains` code, so a
   /// null result means that chain has no endpoint configured on this node and
   /// the caller must fail closed rather than fall back to another client.
   solana_client_entry_ptr get_client(const std::string& id);
   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>& get_idl_files();

   /**
    * @brief Build an `outpost_client` concrete for a Solana outpost.
    *
    * Resolves the shared chain-connection entry by id, filters the plugin's
    * loaded IDLs down to those matching the configured
    * `--solana-outpost-program-name` (default `OPP_SOLANA_OUTPOST_PROGRAM_NAME`),
    * and constructs an `outpost_solana_client` bound to the given program id.
    *
    * @param sol_client_id  Id passed to `--outpost-solana-client`.
    * @param chain_code     Outpost id from `sysio.epoch::outposts`.
    * @param chain_id       Numeric chain id from the outpost row (Solana = 0).
    * @param program_id     Base58 address of the deployed OPP outpost program.
    * @param role           Consumer role; gates the role-specific boot-time
    *                       IDL validation (see `solana_outpost_role`).
    * @throws fc::exception if the client id is unknown, no matching IDL is
    *         loaded, or the role's boot-time IDL validation fails.
    */
   std::shared_ptr<outpost_client> create_outpost_client(const std::string&  sol_client_id,
                                                       uint64_t            chain_code,
                                                       uint32_t            chain_id,
                                                       const std::string&  program_id,
                                                       solana_outpost_role role);

private:
   std::unique_ptr<class outpost_solana_client_plugin_impl> my;
};

} // namespace sysio
