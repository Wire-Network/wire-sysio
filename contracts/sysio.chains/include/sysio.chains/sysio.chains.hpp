#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/system.hpp>
#include <sysio/privileged.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   /**
    * @brief sysio.chains — chain registry on WIRE.
    *
    * Replaces the old `sysio.epoch::outposts` table. Holds one row per Chain
    * (depot + every outpost), keyed by `code` (slug_name). The depot's own row
    * is fixed at `(kind=WIRE, code="WIRE"_s, external_chain_id=0, is_depot=true)`.
    *
    * ## Lifecycle
    *
    * - `regchain(...)`: priv-gated. Inserts row. If `current_epoch_index == 0`
    *   (bootstrap window), sets `active=true` inline. Else `active=false`,
    *   awaiting `activchain`.
    * - `activchain(code)`: priv-gated. Sets `active=true` exactly once.
    *   Reverts on already-active.
    *
    * ## Lookups by sysio.epoch::advance
    *
    * `sysio.epoch::advance` reads `chains` directly via cross-contract KV read
    * to determine the active-outpost fanout list. No mirror table required.
    */
   /**
    * @brief Remote contract identities for one chain's outpost deployment.
    *
    * Grouped into one struct so `regchain` keeps a readable signature and
    * `setoutpost` replaces the whole set atomically. Encoding by kind:
    *  * `EVM`  — each field is a distinct `0x`-prefixed 20-byte hex address:
    *     the OPP contract, the OPPInbound contract, the OperatorRegistry
    *     (the `uw_commit` target), and the contract that emits the source
    *     swap-deposit event scanned by the underwriter's verify path.
    *  * `SVM`  — `opp_addr` is the outpost program id (base58). The single
    *     program serves every role, so the other three MUST be empty; the
    *     daemons substitute `opp_addr` wherever they need one of them.
    *  * `WIRE` — all empty; the depot self-row has no remote deployment.
    *
    * Fields may be empty at registration (the remote contract is not deployed
    * yet) and filled in later via `setoutpost`. Both operator daemons fail
    * closed: a chain whose address they need but do not have is skipped, so an
    * unconfigured row never rides on another chain's deployment.
    */
   struct outpost_addrs {
      std::string opp_addr;
      std::string opp_inbound_addr;
      std::string operator_registry_addr;
      std::string source_deposit_addr;

      SYSLIB_SERIALIZE(outpost_addrs,
         (opp_addr)(opp_inbound_addr)(operator_registry_addr)(source_deposit_addr))
   };

   class [[sysio::contract("sysio.chains")]] chains : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Register a new chain (priv-gated). If called during the bootstrap
      /// window (`current_epoch_index == 0`), inserts with `active=true`
      /// inline; else `active=false`.
      ///
      /// Validation:
      ///  * `code` slug_name format already enforced by the type itself at
      ///     deserialization (alphabet `[A-Z0-9_]+`, ≤8 chars).
      ///  * `code` must be unique.
      ///  * `kind=WIRE` may appear at most once (the depot self-row).
      ///  * `kind=EVM` rows must carry a unique `external_chain_id` — the pair
      ///    `(kind, external_chain_id)` is the outbound envelope's destination
      ///    binding (see `sysio.msgch::buildenv`), verified by EVM outposts
      ///    against their own `block.chainid`.
      ///  * `kind=SVM` may appear at most once — Solana clusters have no
      ///    numeric chain id, so the wire binding is kind-only for SVM and a
      ///    second row would be indistinguishable at the outpost.
      ///  * `outpost` addresses, when non-empty, must match the kind's expected
      ///    format, and fields that are structurally meaningless for the kind
      ///    must be empty. See {@link outpost_addrs}.
      [[sysio::action]]
      void regchain(opp::types::ChainKind kind,
                    sysio::slug_name       code,
                    uint32_t              external_chain_id,
                    std::string           name,
                    std::string           description,
                    outpost_addrs         outpost);

      /// Activate a previously-registered chain (priv-gated, one-shot).
      [[sysio::action]]
      void activchain(sysio::slug_name code);

      /// Replace the remote outpost contract identities for an already-registered
      /// chain (priv-gated). Used when a remote contract is (re)deployed after the
      /// row was registered. Same per-kind encoding and format validation as
      /// `regchain`; rejects the WIRE depot self-row, which has no remote
      /// deployment. The whole set is replaced, so a caller updating one address
      /// must resend the others.
      [[sysio::action]]
      void setoutpost(sysio::slug_name code, outpost_addrs outpost);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      struct chain_key {
         sysio::slug_name code;
         uint64_t primary_key() const { return code.value; }
         SYSLIB_SERIALIZE(chain_key, (code))
      };

      struct [[sysio::table("chains")]] chain_row {
         sysio::slug_name                code;
         opp::types::ChainKind          kind             = opp::types::CHAIN_KIND_UNKNOWN;
         uint32_t                       external_chain_id = 0;
         std::string                    name;
         std::string                    description;
         bool                           is_depot          = false;
         bool                           active            = false;
         uint64_t                       registered_at_ms  = 0;
         uint64_t                       activated_at_ms   = 0;
         /// Remote outpost contract identities for this chain — see {@link outpost_addrs}.
         /// Read by batch_operator_plugin and underwriter_plugin; every operator
         /// therefore relays through the same deployment without per-node config.
         outpost_addrs                  outpost;

         uint64_t by_kind()              const { return magic_enum::enum_integer(kind); }
         uint64_t by_external_chain_id() const { return external_chain_id; }
         uint64_t by_active()            const { return active ? 1 : 0; }

         SYSLIB_SERIALIZE(chain_row,
            (code)(kind)(external_chain_id)(name)(description)
            (is_depot)(active)(registered_at_ms)(activated_at_ms)(outpost))
      };

      using chains_t = sysio::kv::table<"chains"_n, chain_key, chain_row,
         sysio::kv::index<"bykind"_n,    sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_kind>>,
         sysio::kv::index<"byextid"_n,   sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_external_chain_id>>,
         sysio::kv::index<"byactive"_n,  sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_active>>
      >;
   };

} // namespace sysio
