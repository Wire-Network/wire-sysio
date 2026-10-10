#pragma once
/** @file sysio_liq_tester.hpp
 * @brief Shared depot LIQ/swap fixture for yield and treasury gift integration suites.
 */
#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/types/types.pb.h>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"
#include "shadow_yield_reference.hpp"

#include <limits>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;
using sysio_system::test_support::codename_mvo;

/// sysio.liq end to end on the depot: the registries it validates against, the
/// swap it feeds, and sysio.synd as the signer of every mint and burn (an account
/// only; sysio.msgch is an account too, a signer that is not sysio.synd). The
/// treasury supply of WIRE sits on `sysio`, as the bootstrap leaves it.
class sysio_liq_tester : public tester {
public:
   static constexpr auto LIQ_ACCOUNT    = "sysio.liq"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto SWAP_ACCOUNT   = "sysio.swap"_n;
   static constexpr auto SYND_ACCOUNT   = "sysio.synd"_n;
   static constexpr auto SYSIO_ACCOUNT  = "sysio"_n;

   static inline const symbol WIRE_SYM   = symbol::from_string("9,WIRE");
   static inline const symbol LIQSOL_SYM = symbol::from_string("9,LIQSOL");
   static inline const symbol LIQETH_SYM = symbol::from_string("9,LIQETH");
   static inline const symbol POOL_SYM   = symbol::from_string("9,LIQPOOL");
   static constexpr int64_t UNIT = 1'000'000'000;   // one whole token of either symbol, in subunits

   static constexpr std::string_view SOLANA = "SOLANA";
   static constexpr std::string_view ETH    = "ETH";
   static constexpr std::string_view LIQSOL = "LIQSOL";
   static constexpr std::string_view LIQETH = "LIQETH";

   /// Registers the two outposts, their liq tokens and the LIQSOL shadow every case starts from.
   sysio_liq_tester() {
      produce_blocks(2);
      // sysio.synd is the ledger's only minter; an account is enough to sign as it here.
      create_accounts({ LIQ_ACCOUNT, MSGCH_ACCOUNT, TOKEN_ACCOUNT, TOKENS_ACCOUNT, CHAINS_ACCOUNT, SWAP_ACCOUNT,
                        SYND_ACCOUNT, "alice"_n, "bob"_n, "carol"_n, "dave"_n });
      produce_blocks(2);

      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi_ser, true);
      deploy(TOKENS_ACCOUNT, contracts::tokens_wasm(), contracts::tokens_abi(), tokens_abi_ser, true);
      deploy(TOKEN_ACCOUNT,  contracts::token_wasm(),  contracts::token_abi(),  token_abi_ser,  true);
      deploy(SWAP_ACCOUNT,   contracts::swap_wasm(),   contracts::swap_abi(),   swap_abi_ser,   false);
      deploy(LIQ_ACCOUNT,    contracts::liq_wasm(),    contracts::liq_abi(),    liq_abi_ser,    true);

      // WIRE: the treasury supply on sysio, and working balances for the funders.
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, TOKEN_ACCOUNT, "create"_n, mvo()
         ("issuer", SYSIO_ACCOUNT)("maximum_supply", asset(1'000'000'000 * UNIT, WIRE_SYM))));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, SYSIO_ACCOUNT, "issue"_n, mvo()
         ("to", SYSIO_ACCOUNT)("quantity", asset(1'000'000'000 * UNIT, WIRE_SYM))("memo", "")));
      for (auto funder : { "alice"_n, "bob"_n, "carol"_n })
         BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, funder, 1'000'000 * UNIT));

      // The swap: governance is sysio, the system token WIRE.
      BOOST_REQUIRE_EQUAL(success(), push(SWAP_ACCOUNT, swap_abi_ser, SWAP_ACCOUNT, "setconfig"_n, mvo()
         ("fee_authority", SYSIO_ACCOUNT)("system_token", extended_symbol{ WIRE_SYM, TOKEN_ACCOUNT })));

      // Two outposts, each with an active liq token, plus a plain SPL token.
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, SOLANA, 2));
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, ETH, 1));
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, LIQSOL, ChainKind::CHAIN_KIND_SVM, SOLANA));
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, LIQETH, ChainKind::CHAIN_KIND_EVM, ETH));
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_SPL, "USDCSOL", ChainKind::CHAIN_KIND_SVM, SOLANA));

      BOOST_REQUIRE_EQUAL(success(), create(LIQSOL_SYM, SOLANA, LIQSOL));
   }

   // --- deployment and pushing ---

   void deploy(name account, const std::vector<uint8_t>& wasm, const std::vector<char>& abi,
               abi_serializer& ser, bool privileged) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      if (privileged) set_privileged(account);
      produce_blocks();
      sysio_system::test_support::load_account_abi(*this, account, ser);
   }

   action_result push(name code, abi_serializer& ser, name signer, name action_name, const variant_object& data,
                      std::vector<permission_level> authorization = {}) {
      return sysio_system::test_support::push_contract_action_and_produce_block(*this, code, ser, signer,
                                                                                action_name, data,
                                                                                std::move(authorization));
   }
   action_result push_liq(name signer, name action_name, const variant_object& data) {
      return push(LIQ_ACCOUNT, liq_abi_ser, signer, action_name, data);
   }
   static bool mentions(const action_result& r, std::string_view text) {
      return r.find(text) != std::string::npos;
   }

   // --- registries ---

   // Registrations inside the epoch-0 bootstrap window land ACTIVE, as the launch bootstrap's do.
   action_result regchain(ChainKind kind, std::string_view code, uint32_t external_chain_id) {
      return push(CHAINS_ACCOUNT, chains_abi_ser, CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", kind)("code", codename_mvo(code))("external_chain_id", external_chain_id)
         ("name", std::string("outpost"))("description", std::string{})
         ("outpost", sysio_system::test_support::no_outpost_mvo()));
   }
   /// Register a token and bind it to its chain, as the bootstrap does from a `TokenSpec`.
   action_result regtoken(TokenKind kind, std::string_view code, uint32_t precision, ChainKind chain_kind,
                          std::string_view chain_code, const std::vector<char>& address) {
      auto r = push(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT, "regtoken"_n, mvo()
         ("kind", kind)("code", codename_mvo(code))("symbol_name", std::string(code))("description", std::string{})
         ("precision", precision)("address", mvo()("kind", chain_kind)("address", address)));
      if (r != success()) return r;
      return push(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT, "regctok"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(code))("contract_addr", address)("is_native", false));
   }
   /// A 9-decimal token at a placeholder address of the chain family's width.
   action_result regtoken(TokenKind kind, std::string_view code, ChainKind chain_kind, std::string_view chain_code) {
      return regtoken(kind, code, 9, chain_kind, chain_code,
                      std::vector<char>(chain_kind == ChainKind::CHAIN_KIND_SVM ? 32 : 20, char(0x5a)));
   }

   // --- sysio.liq actions ---

   action_result create(symbol sym, std::string_view chain_code, std::string_view token_code, name signer = LIQ_ACCOUNT) {
      return push_liq(signer, "create"_n, mvo()("sym", sym)("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code)));
   }
   /// Mint `amount` of the shadow of `token_code` (LIQSOL by default) to `account`, signed as
   /// sysio.synd, the only minter.
   action_result mint(name account, uint64_t amount, std::string_view token_code = LIQSOL,
                      name signer = SYND_ACCOUNT) {
      return push_liq(signer, "mint"_n, mvo()("to", account)("token_code", codename_mvo(token_code))("amount", amount));
   }
   /// LIQ_YIELD released by sysio.synd: `amount` into the pending balance of `token_code`'s shadow.
   action_result mintyield(std::string_view chain_code, std::string_view token_code, uint64_t amount,
                           name signer = SYND_ACCOUNT) {
      return push_liq(signer, "mintyield"_n, mvo()("chain_code", codename_mvo(chain_code))
         ("token_code", codename_mvo(token_code))("amount", amount));
   }
   action_result queueyield(symbol sym, name signer = "alice"_n) {
      return push_liq(signer, "queueyield"_n, mvo()("sym", sym.to_symbol_code()));
   }
   action_result transfer_shadow(name from, name to, int64_t amount, const std::string& memo = "") {
      return push_liq(from, "transfer"_n, mvo()("from", from)("to", to)("quantity", asset(amount, LIQSOL_SYM))("memo", memo));
   }
   action_result open(name owner, symbol sym, name ram_payer) {
      return push_liq(ram_payer, "open"_n, mvo()("owner", owner)("symbol", sym)("ram_payer", ram_payer));
   }
   action_result close(name owner, symbol sym) {
      return push_liq(owner, "close"_n, mvo()("owner", owner)("symbol", sym));
   }
   action_result claim(name holder, symbol sym = LIQSOL_SYM) {
      return push_liq(holder, "claim"_n, mvo()("holder", holder)("sym", sym.to_symbol_code()));
   }
   action_result addyield(name from, int64_t wire_amount, symbol target = LIQSOL_SYM) {
      return push_liq(from, "addyield"_n, mvo()("from", from)("quantity", asset(wire_amount, WIRE_SYM))
         ("target", target.to_symbol_code()));
   }
   action_result recredit(name holder, int64_t amount, name signer = LIQ_ACCOUNT) {
      return push_liq(signer, "recredit"_n, mvo()("holder", holder)("quantity", asset(amount, LIQSOL_SYM)));
   }
   action_result regliqpool(std::string_view chain_code, std::string_view token_code, symbol pair_symbol,
                            uint64_t initial_chain_amount, uint64_t initial_wire_amount, int32_t fee = 30,
                            int64_t locked_shares = 0, uint32_t horizon_sec = 86400, uint32_t depth_cap_bps = 300,
                            int64_t clip_floor = 1000, name signer = LIQ_ACCOUNT) {
      return push_liq(signer, "regliqpool"_n, mvo()("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))
         ("pair_symbol", pair_symbol)("initial_chain_amount", initial_chain_amount)
         ("initial_wire_amount", initial_wire_amount)("fee", fee)("locked_shares", locked_shares)
         ("conversion_horizon_sec", horizon_sec)("depth_cap_bps", depth_cap_bps)("clip_floor", clip_floor));
   }
   // --- other contracts ---

   action_result transfer_wire(name from, name to, int64_t amount) {
      return push(TOKEN_ACCOUNT, token_abi_ser, from, "transfer"_n, mvo()
         ("from", from)("to", to)("quantity", asset(amount, WIRE_SYM))("memo", ""));
   }
   action_result tickyield(symbol pair = POOL_SYM, name signer = "alice"_n) {
      return push(SWAP_ACCOUNT, swap_abi_ser, signer, "tickyield"_n, mvo()("pair_token", pair.to_symbol_code()));
   }
   // The swap is unprivileged and bills a user's rows to the user, so these carry the
   // user's `sysio.payer` beside `active`, as the swap suite's own pushes do.
   /// A user's deposit row on the swap for one token, RAM billed to `payer`.
   action_result openext(name user, name payer, const extended_symbol& ext_symbol) {
      return push(SWAP_ACCOUNT, swap_abi_ser, payer, "openext"_n, mvo()
         ("user", user)("payer", payer)("ext_symbol", ext_symbol),
         sysio_system::test_support::payer_authorization(payer));
   }
   /// Sell `ext_asset_in` from the user's deposit into `pair` for at least `min_expected`.
   action_result exchange(name user, symbol pair, const extended_asset& ext_asset_in, const asset& min_expected) {
      return push(SWAP_ACCOUNT, swap_abi_ser, user, "exchange"_n, mvo()
         ("user", user)("pair_token", pair.to_symbol_code())("ext_asset_in", ext_asset_in)("min_expected", min_expected),
         sysio_system::test_support::payer_authorization(user));
   }
   /// Move `to_withdraw` from the user's deposit on the swap to `to`'s wallet.
   action_result withdraw(name user, name to, const extended_asset& to_withdraw) {
      return push(SWAP_ACCOUNT, swap_abi_ser, user, "withdraw"_n, mvo()
         ("user", user)("to", to)("to_withdraw", to_withdraw)("memo", ""),
         sysio_system::test_support::payer_authorization(user));
   }

   // --- rows ---

   fc::variant decode(abi_serializer& ser, const char* type, const std::vector<char>& data) {
      return data.empty() ? fc::variant()
                          : ser.binary_to_variant(type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   fc::variant liq_row(name table, const char* type, name scope, uint64_t id) {
      return decode(liq_abi_ser, type, get_row_by_id(LIQ_ACCOUNT, scope, table, id));
   }
   fc::variant account_row(name holder, symbol sym = LIQSOL_SYM) {
      return liq_row("accounts"_n, "account", holder, sym.to_symbol_code().value);
   }
   fc::variant index_row(symbol sym = LIQSOL_SYM) {
      return liq_row("yieldidx"_n, "yield_index", LIQ_ACCOUNT, sym.to_symbol_code().value);
   }
   fc::variant stat_row(symbol sym = LIQSOL_SYM) {
      return liq_row("stat"_n, "currency_stats", LIQ_ACCOUNT, sym.to_symbol_code().value);
   }
   fc::variant pending_row(symbol sym = LIQSOL_SYM) {
      return liq_row("liqpending"_n, "pending_yield", LIQ_ACCOUNT, sym.to_symbol_code().value);
   }
   int64_t wire_balance(name holder) {
      const auto row = decode(token_abi_ser, "account",
                              get_row_by_id(TOKEN_ACCOUNT, holder, "accounts"_n, WIRE_SYM.to_symbol_code().value));
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }
   int64_t shadow_balance(name holder) {
      const auto row = account_row(holder);
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }
   int64_t supply() { return stat_row()["supply"].as<asset>().get_amount(); }
   fc::uint128_t index() {
      const auto row = index_row();
      return row.is_null() ? fc::uint128_t{ 0 } : row["index"].as_uint128();
   }
   uint64_t pot() {
      const auto row = index_row();
      return row.is_null() ? 0 : row["pot"].as_uint64();
   }
   /// What the token's spec says `holder` is owed now.
   int64_t owed(name holder) {
      const auto row = account_row(holder);
      if (row.is_null()) return 0;
      return yield_reference::owed(row["balance"].as<asset>().get_amount(), index(),
                                   row["index_checkpoint"].as_uint128(), row["owed_wire"].as_uint64());
   }
   fc::variant swap_row(name table, const char* type, name scope, uint64_t id) {
      return decode(swap_abi_ser, type, get_row_by_id(SWAP_ACCOUNT, scope, table, id));
   }
   int64_t reservoir() {
      const auto row = swap_row("reservoirs"_n, "reservoir", SWAP_ACCOUNT, POOL_SYM.to_symbol_code().value);
      return row.is_null() ? -1 : row["balance"]["quantity"].as<asset>().get_amount();
   }
   /// The swap's `stat` row of a pair: `pool1` is the shadow leg, `pool2` the WIRE leg.
   fc::variant swap_pool_row(symbol pair) {
      return swap_row("stat"_n, "currency_stats", SWAP_ACCOUNT, pair.to_symbol_code().value);
   }
   abi_serializer liq_abi_ser, token_abi_ser, tokens_abi_ser, chains_abi_ser, swap_abi_ser;
};
