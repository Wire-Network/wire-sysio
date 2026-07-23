#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>

namespace sysio {
using namespace fc::network::solana;

/// Default program name used in the Anchor IDL for the Solana OPP outpost.
/// Shared between the outpost_solana_client_plugin and the batch_operator_plugin
/// so both speak a single constant when locating the program's IDL entry.
/// Overridable at runtime via `--solana-outpost-program-name`: the clean-room
/// outpost implementation is hosted inside the `liqsol_core` program, whose
/// generated IDL carries that name instead of `opp_outpost`.
inline constexpr const char* OPP_SOLANA_OUTPOST_PROGRAM_NAME = "opp_outpost";

/// Interval between successive `getSignaturesForAddress` + log-scan attempts
/// inside the underwriter daemon's `verify_source_deposit_sol`. Kept long
/// enough to keep the RPC load production-acceptable — a tighter interval is
/// reserved for tighter-budget flows (e.g. tx-confirmation polling at the
/// `processed` commitment level, ~400ms).
///
/// Shared with the underwriter plugin so the production-tuning lever lives
/// in one place per chain client.
inline constexpr auto SOL_SWAP_DEPOSIT_POLL_INTERVAL = fc::seconds(15);

/// Total wall-clock budget for `verify_source_deposit_sol` to find the
/// `SwapDeposit` marker log line emitted by `opp-outpost::request_swap`.
/// On expiry the verifier returns `false` and the underwriter's outer poll
/// loop reattempts on its next tick. 120s comfortably covers:
///   - the slot it took for `request_swap` to land + finalize, AND
///   - the RPC `getSignaturesForAddress` window (default ~1000 sigs back),
///   - across an `solana-test-validator` cluster (no ledger pruning within
///     this horizon) and a production RPC (≥ 2 epochs of tx history).
inline constexpr auto SOL_SWAP_DEPOSIT_TOTAL_TIMEOUT = fc::seconds(120);

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
   /// Only submits `uw_commit`; never reads inbound envelopes, so the
   /// `LatestOutboundEnvelope` boot assertion is skipped. If such a client
   /// ever does call `read_inbound_envelope` against an IDL without the
   /// account, the read logs a warning and yields no envelope.
   underwriter
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
   /// Outpost lamport vault. Holds escrowed collateral deposited via
   /// `deposit`; drained on inbound WITHDRAW_REMIT / SLASH /
   /// DEPOSIT_REVERT by the program's signed system_program::transfer
   /// CPI. Pre-derived from seed `outpost_vault`.
   fc::network::solana::solana_public_key vault_pda;
   /// Outpost Reserve PDA — receives slashed-collateral routing and
   /// DEPOSIT_REVERT penalties. Pre-derived from seed `outpost_reserve`.
   fc::network::solana::solana_public_key reserve_pda;

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
   /// Anchor `remaining_accounts`. The cranker
   /// (`outpost_solana_client::deliver_outbound_envelope`) decodes the
   /// committed envelope and builds account metas for every effect account the
   /// terminal handlers may touch: operator/depositor wallets, Reserve PDAs,
   /// SPL vaults, canonical ATAs, mints, and token/ATA/system programs. Data
   /// chunks ignore the slice — only the zero-data terminal call's account
   /// list matters for dispatch.
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
   /// never calls it because the consensus-reaching terminal `epoch_in` emits
   /// the outbound envelope inline.
   solana_program_tx_fn<std::string, uint32_t>             emit_outbound_envelope;
   /// `deposit(operator_type: u8, wire_account_name: string, amount: u64) -> signature`.
   solana_program_tx_fn<std::string, uint8_t, std::string, uint64_t> deposit;
   /// `commit_underwrite(uic_bytes: bytes) -> signature`.
   /// Relays an underwriter's signed `UnderwriteIntentCommit` to the
   /// outpost as opaque bytes. The on-chain handler stores the bytes
   /// for the next outbound envelope so the batch operator can relay
   /// the COMMIT back to the depot; no other state changes.
   solana_program_tx_fn<std::string, std::vector<uint8_t>> commit_underwrite;

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
      , vault_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'o','u','t','p','o','s','t','_','v','a','u','l','t'}},
           prog_id).first)
      // v6: SOL outpost reserve aggregate is seeded with b"reserve_aggregate"
      // (see `RESERVE_AGGREGATE_SEED` in programs/opp-outpost/src/state/reserve.rs).
      // Previously this used b"outpost_reserve" which derived to a non-existent
      // PDA → epoch_in's `reserve_aggregate` account validation failed with
      // fc::assert_exception 10.
      , reserve_pda(fc::network::solana::system::find_program_address(
           {std::vector<uint8_t>{'r','e','s','e','r','v','e','_','a','g','g','r','e','g','a','t','e'}},
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
           // The Solana program's `epoch_in` finalize path now fires the
           // outbound emit inline at consensus reach (see
           // `epoch_in.rs::finalize_envelope`), so the IDL's account list
           // grew to carry the outbound-emit accounts. The relay injects
           // pre-derived PDAs for all of them; the operator never sends a
           // separate `emit_outbound_envelope` tx.
           account_overrides_t overrides = {
              {"config",                    config_pda},
              {"operator_registry",         operator_registry_pda},
              {"epoch_deliveries",          epoch_deliveries_pda},
              {"chunk_buffer",              chunk_buffer_pda},
              {"inbound_envelopes",         inbound_envelopes_pda},
              {"outbound_message_buffer",   outbound_message_buffer_pda},
              {"outbound_envelopes",        outbound_envelopes_pda},
              {"latest_outbound_envelope",  latest_outbound_envelope_pda},
              {"vault",                     vault_pda},
              // v6 IDL field is `reserve_aggregate` (matches the Anchor
              // `#[derive(Accounts)]` field name in epoch_in.rs / Initialize).
              {"reserve_aggregate",         reserve_pda},
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
           // terminal call — that's the call that triggers `finalize_envelope` +
           // `emit_outbound_inner` on the Solana side, which compounds:
           //   * Anchor deserialise of 9 mut accounts (~5–7 KiB heap)
           //   * `chunk_buffer.data` manual read + envelope_data clone (~5 KiB)
           //   * decoded Envelope proto + nested attestation Vecs (~5–10 KiB)
           //   * inline message processing temp allocs + emit_outbound encode
           // peak ≈ 22–30 KiB, right at the 32 KiB default BPF heap.
           // Bounded +88 B/epoch growth in the inbound + outbound EnvelopeLog
           // Vecs tipped the cluster over the heap ceiling at epoch 13 (OOM
           // panic in `log_info.resize`); bumping to 256 KiB on this one tx
           // gives the finalize path 8× margin and removes the failure mode.
           //
           // Non-final chunks just `chunk_buffer.data.extend_from_slice(...)`
           // and consume <5 KiB heap — they don't need the bump, and
           // injecting pre-ixs there bloats the chunk-write tx past the
           // 1 232-byte MTU (the bug we hit earlier with always-on pre-ixs;
           // the encoded tx hit 1 744 > 1 644 cap).
           //
           // Deliberately NOT injecting `set_compute_unit_limit` — the
           // OOM tx consumed 116 K of 200 K CU, so CU is not the
           // bottleneck for the production 2.5 KB envelope. Add a CU
           // bump only when 64 KB envelopes land live.
           std::vector<fc::network::solana::instruction> pre_ixs;
           if (chunk_index == total_chunks) {
              pre_ixs.push_back(
                 fc::network::solana::system::compute_budget::request_heap_frame(256'000));
           }
           // Resolve the IDL's declared accounts first, then append any
           // extra `remaining_accounts` the cranker decoded from the
           // inbound envelope (operator / depositor wallets, reserve PDAs,
           // SPL vaults, and token programs that effect handlers need to
           // address).
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
      , deposit(create_tx_and_confirm<std::string, uint8_t, std::string, uint64_t>(get_idl("deposit")))
      // commit_underwrite is `(uic_bytes: bytes) -> signature`. The IDL declares
      // three accounts — `underwriter` (signer, default-resolved from the
      // client), `operator_registry` (PDA), and `outbound_message_buffer`
      // (PDA). IDL v2 (Anchor 0.31+) does not embed PDA seeds, so the typed
      // wrapper must inject the pre-derived PDAs as overrides — same pattern
      // as `epoch_in` / `emit_outbound_envelope` above.
      , commit_underwrite([this](std::vector<uint8_t> uic_bytes) -> std::string {
           account_overrides_t overrides = {
              {"operator_registry",        operator_registry_pda},
              {"outbound_message_buffer",  outbound_message_buffer_pda},
           };
           auto& instr = get_idl("commit_underwrite");
           program_invoke_data_items params = {fc::variant(uic_bytes)};
           return execute_tx_and_confirm(instr, resolve_accounts(instr, params, overrides), params);
        }) {}
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
