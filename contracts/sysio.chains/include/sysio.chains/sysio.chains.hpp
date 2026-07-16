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
      /// Validation:
      ///  * `code` slug_name format already enforced by the type itself at
      ///     deserialization (alphabet `[A-Z0-9_]+`, ≤8 chars).
      ///  * `code` must be unique.
      ///  * `kind=WIRE` may appear at most once (the depot self-row).
      ///  * `kind=EVM` rows must carry a unique `external_chain_id` — the pair
      ///    `(kind, external_chain_id)` is the outbound envelope's destination
      ///    binding (see `sysio.msgch::buildenv`), verified by EVM outposts
      ///    against their own `block.chainid`.
      [[sysio::action]]
      void regchain(opp::types::ChainKind kind,
                    sysio::slug_name       code,
                    uint32_t              external_chain_id,
                    std::string           name,
                    std::string           description);

      /// Activate a previously-registered chain (priv-gated, one-shot).
      [[sysio::action]]
      void activchain(sysio::slug_name code);

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

         uint64_t by_kind()              const { return magic_enum::enum_integer(kind); }
         uint64_t by_external_chain_id() const { return external_chain_id; }
         uint64_t by_active()            const { return active ? 1 : 0; }

         SYSLIB_SERIALIZE(chain_row,
            (code)(kind)(external_chain_id)(name)(description)
            (is_depot)(active)(registered_at_ms)(activated_at_ms))
      };

      using chains_t = sysio::kv::table<"chains"_n, chain_key, chain_row,
         sysio::kv::index<"bykind"_n,    sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_kind>>,
         sysio::kv::index<"byextid"_n,   sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_external_chain_id>>,
         sysio::kv::index<"byactive"_n,  sysio::const_mem_fun<chain_row, uint64_t, &chain_row::by_active>>
      >;
   };

} // namespace sysio
