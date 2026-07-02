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
      /// `opp_addr` / `opp_inbound_addr` bind this chain code to the exact
      /// remote OPP contracts, so a batch operator relaying two same-kind
      /// outposts cannot collapse them onto one shared remote endpoint. The
      /// binding is a consensus fact (every operator reads the same row), which
      /// also lets the remote outpost reject an envelope delivered under a
      /// mismatched WIRE chain code. Encoding by kind:
      ///  * `EVM`  — `opp_addr` = OPP contract, `opp_inbound_addr` = OPPInbound
      ///     contract, each a `0x`-prefixed 20-byte hex address.
      ///  * `SVM`  — `opp_addr` = the outpost program id (base58); the single
      ///     program serves both directions, so `opp_inbound_addr` is empty.
      ///  * `WIRE` — both empty (the depot self-row has no remote endpoint).
      ///
      /// Addresses may be left empty at registration (e.g. the remote contract
      /// is not deployed yet) and filled in later via `setoutpost`; the batch
      /// operator fail-closed skips any active outpost whose addresses are not
      /// yet set, so an unconfigured row never rides on another chain's endpoint.
      ///
      /// Validation:
      ///  * `code` slug_name format already enforced by the type itself at
      ///     deserialization (alphabet `[A-Z0-9_]+`, ≤8 chars).
      ///  * `code` must be unique.
      ///  * `kind=WIRE` may appear at most once (the depot self-row).
      ///  * addresses, when non-empty, must match the kind's expected format.
      [[sysio::action]]
      void regchain(opp::types::ChainKind kind,
                    sysio::slug_name       code,
                    uint32_t              external_chain_id,
                    std::string           name,
                    std::string           description,
                    std::string           opp_addr,
                    std::string           opp_inbound_addr);

      /// Activate a previously-registered chain (priv-gated, one-shot).
      [[sysio::action]]
      void activchain(sysio::slug_name code);

      /// Update the remote OPP contract binding for an already-registered
      /// chain (priv-gated). Used when a remote contract is (re)deployed after
      /// the row was registered. Same per-kind encoding and format validation
      /// as `regchain`; rejects the WIRE depot self-row (no remote endpoint).
      [[sysio::action]]
      void setoutpost(sysio::slug_name code,
                      std::string      opp_addr,
                      std::string      opp_inbound_addr);

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
         /// Exact remote OPP contract binding for this chain code (see
         /// `regchain`). EVM: `opp_addr` = OPP, `opp_inbound_addr` = OPPInbound
         /// (0x-hex). SVM: `opp_addr` = program id (base58), `opp_inbound_addr`
         /// empty. WIRE: both empty.
         std::string                    opp_addr;
         std::string                    opp_inbound_addr;

         uint64_t by_kind()              const { return magic_enum::enum_integer(kind); }
         uint64_t by_external_chain_id() const { return external_chain_id; }
         uint64_t by_active()            const { return active ? 1 : 0; }

         SYSLIB_SERIALIZE(chain_row,
            (code)(kind)(external_chain_id)(name)(description)
            (is_depot)(active)(registered_at_ms)(activated_at_ms)
            (opp_addr)(opp_inbound_addr))
      };

      using chains_t = sysio::kv::table<"chains"_n, chain_key, chain_row,
         sysio::kv::index<"bykind"_n,    sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_kind>>,
         sysio::kv::index<"byextid"_n,   sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_external_chain_id>>,
         sysio::kv::index<"byactive"_n,  sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_active>>
      >;
   };

} // namespace sysio
