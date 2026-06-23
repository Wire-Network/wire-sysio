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
    * @brief sysio.tokens — token registry + per-chain token bindings.
    *
    * Two tables:
    *
    *  * `tokens` — chain-independent token concepts. PK = `code.value` (slug_name).
    *    `Token.address` (ChainAddress) carries the canonical chain-of-origin
    *    contract address for non-native tokens (LIQ-class, ERC20s, etc.);
    *    empty for NATIVE-kind tokens.
    *
    *  * `chaintokens` — (Chain, Token) binding with chain-specific attributes.
    *    Composite PK = uint128 `(chain_code << 64) | token_code`. Carries
    *    `contract_addr` (the per-outpost address — equal to `Token.address`
    *    bytes for the chain-of-origin binding, distinct for wrapped versions
    *    on other chains), `precision_override`, and `is_native` (exactly one
    *    per Chain).
    *
    * ## Lifecycle (unified bootstrap + post-bootstrap, per
    * /data/shared/code/wire/.claude/rules/epoch-duration-global.md):
    *
    *  * `regtoken` / `regchaintoken`: priv-gated. If
    *    `current_epoch_index == 0` (bootstrap window) sets `active=true`
    *    inline; else `active=false`.
    *  * `activtoken` / `activchaintoken`: priv-gated. Sets `active=true`
    *    exactly once. `activchaintoken` additionally enforces "exactly one
    *    `is_native=true` per Chain" at activation time.
    */
   class [[sysio::contract("sysio.tokens")]] tokens : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      [[sysio::action]]
      void regtoken(opp::types::TokenKind    kind,
                    sysio::slug_name          code,
                    std::string              symbol_name,
                    std::string              description,
                    uint32_t                 precision,
                    opp::types::ChainAddress address);

      [[sysio::action]]
      void activtoken(sysio::slug_name code);

      /// @param precision_override Chain-native decimal precision for this
      ///        (chain, token) binding. Defaults to the canonical 9-decimal
      ///        depot frame; pass a different value only when the origin chain
      ///        represents the asset with a different number of decimals
      ///        (e.g. a 6-decimal ERC-20). Stored as metadata; the depot's
      ///        on-chain accounting always operates in the canonical 9-decimal
      ///        frame.
      [[sysio::action]]
      void regctok(sysio::slug_name   chain_code,
                   sysio::slug_name   token_code,
                   std::vector<char> contract_addr,
                   uint32_t          precision_override,
                   bool              is_native);

      [[sysio::action]]
      void activctok(sysio::slug_name chain_code, sysio::slug_name token_code);

      // -----------------------------------------------------------------------
      //  Tables: tokens
      // -----------------------------------------------------------------------

      struct token_key {
         sysio::slug_name code;
         uint64_t primary_key() const { return code.value; }
         SYSLIB_SERIALIZE(token_key, (code))
      };

      struct [[sysio::table("tokens")]] token_row {
         sysio::slug_name             code;
         opp::types::TokenKind       kind             = opp::types::TOKEN_KIND_UNKNOWN;
         std::string                 symbol_name;
         std::string                 description;
         uint32_t                    precision        = 9;
         opp::types::ChainAddress    address;
         bool                        active           = false;
         uint64_t                    registered_at_ms = 0;
         uint64_t                    activated_at_ms  = 0;

         uint64_t by_kind()   const { return magic_enum::enum_integer(kind); }
         uint64_t by_active() const { return active ? 1 : 0; }

         SYSLIB_SERIALIZE(token_row,
            (code)(kind)(symbol_name)(description)(precision)(address)
            (active)(registered_at_ms)(activated_at_ms))
      };

      using tokens_t = sysio::kv::table<"tokens"_n, token_key, token_row,
         sysio::kv::index<"bykind"_n,    sysio::const_mem_fun<token_row, uint64_t, &token_row::by_kind>>,
         sysio::kv::index<"byactive"_n,  sysio::const_mem_fun<token_row, uint64_t, &token_row::by_active>>
      >;

      // -----------------------------------------------------------------------
      //  Tables: chaintokens
      // -----------------------------------------------------------------------

      struct chain_token_key {
         sysio::slug_name chain_code;
         sysio::slug_name token_code;
         uint128_t primary_key() const {
            return (static_cast<uint128_t>(chain_code.value) << 64) | token_code.value;
         }
         SYSLIB_SERIALIZE(chain_token_key, (chain_code)(token_code))
      };

      struct [[sysio::table("chaintokens")]] chain_token_row {
         sysio::slug_name   chain_code;
         sysio::slug_name   token_code;
         std::vector<char> contract_addr;
         // Chain-native decimal precision for this binding. Defaults to the
         // canonical 9-decimal depot frame so every token is 9 decimals unless
         // an origin chain explicitly represents it differently.
         uint32_t          precision_override = 9;
         bool              is_native          = false;
         bool              active             = false;
         uint64_t          registered_at_ms   = 0;
         uint64_t          activated_at_ms    = 0;

         uint64_t by_chain_code() const { return chain_code.value; }
         uint64_t by_token_code() const { return token_code.value; }
         uint64_t by_active()     const { return active ? 1 : 0; }

         SYSLIB_SERIALIZE(chain_token_row,
            (chain_code)(token_code)(contract_addr)(precision_override)
            (is_native)(active)(registered_at_ms)(activated_at_ms))
      };

      using chaintokens_t = sysio::kv::table<"chaintokens"_n, chain_token_key, chain_token_row,
         sysio::kv::index<"bychain"_n, sysio::const_mem_fun<chain_token_row, uint64_t, &chain_token_row::by_chain_code>>,
         sysio::kv::index<"bytoken"_n, sysio::const_mem_fun<chain_token_row, uint64_t, &chain_token_row::by_token_code>>,
         sysio::kv::index<"byactive"_n, sysio::const_mem_fun<chain_token_row, uint64_t, &chain_token_row::by_active>>
      >;
   };

} // namespace sysio
