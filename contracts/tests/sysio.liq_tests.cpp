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

namespace {

/// The action data of `sysio.system::fundclaim(name recipient, int64_t amount)`, for
/// reading the kicker request out of an addyield trace.
struct fundclaim_args {
   name    recipient;
   int64_t amount;
};

} // anonymous namespace
FC_REFLECT( fundclaim_args, (recipient)(amount) )

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
   action_result addkicker(name signer, symbol sym, int64_t base_balance, uint64_t requested) {
      return push_liq(signer, "addkicker"_n, mvo()("sym", sym.to_symbol_code())("base_balance", base_balance)
         ("requested", requested));
   }
   action_result setkicker(name signer, uint32_t bps) {
      return push_liq(signer, "setkicker"_n, mvo()("bps", bps));
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
   fc::variant config_row() {
      return decode(liq_abi_ser, "liq_config", get_row_by_account(LIQ_ACCOUNT, LIQ_ACCOUNT, "liqconfig"_n, "liqconfig"_n));
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

BOOST_AUTO_TEST_SUITE(sysio_liq_tests)

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(create_binds_an_active_liq_token, sysio_liq_tester) try {
   const auto st = stat_row();
   BOOST_REQUIRE_EQUAL(0, st["supply"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(SOLANA, st["chain_code"].as_string());
   BOOST_REQUIRE_EQUAL(LIQSOL, st["token_code"].as_string());

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code already has a shadow symbol"),
                       create(symbol::from_string("9,LIQSOLB"), SOLANA, LIQSOL));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code is not an active registry token"),
                       create(LIQETH_SYM, ETH, "NOPE"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code is not a liq token"),
                       create(symbol::from_string("9,USDCSOL"), SOLANA, "USDCSOL"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("symbol precision must match the token's depot precision"),
                       create(symbol::from_string("4,LIQETH"), ETH, LIQETH));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code is not bound to chain_code"),
                       create(LIQETH_SYM, SOLANA, LIQETH));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("symbol already exists"),
                       create(LIQSOL_SYM, ETH, LIQETH));
   BOOST_REQUIRE(mentions(create(LIQETH_SYM, ETH, LIQETH, "alice"_n), "missing authority of sysio.liq"));
   BOOST_REQUIRE_EQUAL(success(), create(LIQETH_SYM, ETH, LIQETH));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Inbound: SYNDICATE_LIQ
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(mint_credits_a_holder_and_refuses_what_it_must, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   auto row = account_row("alice"_n);
   BOOST_REQUIRE_EQUAL(100 * UNIT, row["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{ 0 }, row["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(0u, row["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(100 * UNIT, supply());

   // Every refusal aborts and changes nothing: an unknown token, a zero amount, an account that
   // does not exist, and any signer but sysio.synd.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), mint("alice"_n, 5 * UNIT, "NOPE"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("amount must be positive"), mint("alice"_n, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("to account does not exist"), mint("nobody"_n, 5 * UNIT));
   BOOST_REQUIRE(mentions(mint("alice"_n, UNIT, LIQSOL, MSGCH_ACCOUNT), "missing authority of sysio.synd"));
   BOOST_REQUIRE_EQUAL(100 * UNIT, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(100 * UNIT, supply());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The token: settle before mutate
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(settle_before_mutate_conserves_every_subunit, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, 300 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(25 * UNIT, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(75 * UNIT, owed("bob"_n));

   // A transfer mid-period banks each side's accrual and restamps both rows.
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "bob"_n, 50 * UNIT));
   auto alice = account_row("alice"_n);
   auto bob   = account_row("bob"_n);
   BOOST_REQUIRE_EQUAL(uint64_t(25 * UNIT), alice["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(uint64_t(75 * UNIT), bob["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(index(), alice["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(index(), bob["index_checkpoint"].as_uint128());

   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(25 * UNIT + 12'500'000'000, owed("alice"_n));   // 50 of 400
   BOOST_REQUIRE_EQUAL(75 * UNIT + 87'500'000'000, owed("bob"_n));     // 350 of 400
   BOOST_REQUIRE_EQUAL(owed("alice"_n) + owed("bob"_n), 200 * UNIT);

   // Both claims pay what the spec says and empty the pot.
   const int64_t alice_before = wire_balance("alice"_n), bob_before = wire_balance("bob"_n);
   const int64_t alice_owed = owed("alice"_n), bob_owed = owed("bob"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim("bob"_n));
   BOOST_REQUIRE_EQUAL(alice_before + alice_owed, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(bob_before + bob_owed, wire_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(0u, pot());
   BOOST_REQUIRE_EQUAL(0, owed("alice"_n));

   // A row opened after the index moved is stamped, not zeroed: nothing is owed,
   // claiming sends nothing, and a later credit accrues only from here.
   BOOST_REQUIRE_EQUAL(success(), open("dave"_n, LIQSOL_SYM, "dave"_n));
   BOOST_REQUIRE_EQUAL(index(), account_row("dave"_n)["index_checkpoint"].as_uint128());
   const int64_t dave_before = wire_balance("dave"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("dave"_n));
   BOOST_REQUIRE_EQUAL(dave_before, wire_balance("dave"_n));
   BOOST_REQUIRE_EQUAL(success(), mint("dave"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(0, owed("dave"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 50 * UNIT));   // 500 supply: dave holds a fifth
   BOOST_REQUIRE_EQUAL(10 * UNIT, owed("dave"_n));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"), transfer_shadow("dave"_n, "alice"_n, 101 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no balance object found"), transfer_shadow("carol"_n, "alice"_n, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("yield must be in WIRE"),
                       push_liq("carol"_n, "addyield"_n, mvo()("from", "carol"_n)("quantity", asset(UNIT, LIQSOL_SYM))
                                                             ("target", LIQSOL_SYM.to_symbol_code())));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(a_one_unit_distribution_carries_its_remainder, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 3));   // three subunits
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 1));
   BOOST_REQUIRE_EQUAL(1u, index_row()["carry"].as_uint64());
   BOOST_REQUIRE_EQUAL(0, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 1));
   BOOST_REQUIRE_EQUAL(2u, index_row()["carry"].as_uint64());
   BOOST_REQUIRE_EQUAL(1, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 1));
   BOOST_REQUIRE_EQUAL(0u, index_row()["carry"].as_uint64());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{ yield_reference::Scale }, index());
   BOOST_REQUIRE_EQUAL(3, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(3u, pot());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(claim_with_nothing_owed_sends_nothing, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   const int64_t before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(0, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(before, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, account_row("alice"_n)["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(100 * UNIT, shadow_balance("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(close_refuses_while_yield_is_owed, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("Cannot close because the balance is not zero."), close("alice"_n, LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "bob"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(0, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("Cannot close because yield is still owed; claim first."), close("alice"_n, LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), close("alice"_n, LIQSOL_SYM));
   BOOST_REQUIRE(account_row("alice"_n).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no balance object found"), claim("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(the_index_grows_past_64_bits, sysio_liq_tester) try {
   // One subunit of supply and 2e7 subunits of yield move the index by 2e19.
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 1));
   const int64_t donation = 20'000'000;
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, donation));
   BOOST_REQUIRE_LT(yield_reference::wide(std::numeric_limits<uint64_t>::max()), yield_reference::wide(index()));
   BOOST_REQUIRE_EQUAL(donation, owed("alice"_n));
   const int64_t before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(before + donation, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(index(), account_row("alice"_n)["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, donation));
   BOOST_REQUIRE_EQUAL(donation, owed("alice"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The kicker
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(addyield_requests_the_kicker_from_t5, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(uint32_t(200), config_row().is_null() ? 200u : config_row()["kicker_bps"].as<uint32_t>());

   // The swap's intake is the one that is yield, so it is the one that earns the
   // kicker. Stand in for the pool's proceeds: WIRE deposited on the swap.
   BOOST_REQUIRE_EQUAL(success(), openext("alice"_n, "alice"_n, extended_symbol{ WIRE_SYM, TOKEN_ACCOUNT }));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("alice"_n, SWAP_ACCOUNT, 300 * UNIT));
   // 100 WIRE in from `from`; whether the trace carries the 2% fundclaim for
   // this contract and the fold that follows it.
   const auto intake = [&](name from) {
      auto trace = base_tester::push_action(LIQ_ACCOUNT, "addyield"_n, from, mvo()
         ("from", from)("quantity", asset(100 * UNIT, WIRE_SYM))("target", LIQSOL_SYM.to_symbol_code()));
      produce_block();
      bool requested = false, folded = false;
      for (const auto& at : trace->action_traces) {
         if (at.act.account == SYSIO_ACCOUNT && at.act.name == "fundclaim"_n) {
            const auto args = fc::raw::unpack<fundclaim_args>(at.act.data);
            BOOST_REQUIRE_EQUAL(LIQ_ACCOUNT, args.recipient);
            BOOST_REQUIRE_EQUAL(2 * UNIT, args.amount);
            requested = true;
         }
         if (at.act.account == LIQ_ACCOUNT && at.act.name == "addkicker"_n) folded = true;
      }
      return std::make_pair(requested, folded);
   };

   // sysio runs no emissions here, so the request lands nothing and the fold is
   // a no-op: holders get the base yield and nothing else.
   BOOST_REQUIRE(intake(SWAP_ACCOUNT) == std::make_pair(true, true));
   BOOST_REQUIRE_EQUAL(100 * UNIT, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(uint64_t(100 * UNIT), pot());

   // A donation from anyone else distributes only itself: no request, no fold.
   // Otherwise a near-sole holder could donate, claim it back with the kicker on
   // top, and repeat against the treasury.
   BOOST_REQUIRE(intake("carol"_n) == std::make_pair(false, false));
   BOOST_REQUIRE_EQUAL(200 * UNIT, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(uint64_t(200 * UNIT), pot());

   // With the kicker off, the swap's intake makes no request either.
   BOOST_REQUIRE(mentions(setkicker("alice"_n, 0), "missing authority of sysio"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("kicker_bps out of range"), setkicker(SYSIO_ACCOUNT, 10'001));
   BOOST_REQUIRE_EQUAL(success(), setkicker(SYSIO_ACCOUNT, 0));
   BOOST_REQUIRE_EQUAL(0u, config_row()["kicker_bps"].as<uint32_t>());
   BOOST_REQUIRE(intake(SWAP_ACCOUNT) == std::make_pair(false, false));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(addkicker_folds_only_what_landed, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   // Stand in for T5: 5 WIRE arrive at the contract.
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("carol"_n, LIQ_ACCOUNT, 5 * UNIT));
   const int64_t balance = wire_balance(LIQ_ACCOUNT);

   BOOST_REQUIRE(mentions(addkicker("alice"_n, LIQSOL_SYM, balance - 2 * UNIT, 3 * UNIT), "missing authority of sysio.liq"));
   // 2 WIRE over the base, 3 requested: 2 fold in.
   BOOST_REQUIRE_EQUAL(success(), addkicker(LIQ_ACCOUNT, LIQSOL_SYM, balance - 2 * UNIT, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(uint64_t(2 * UNIT), pot());
   BOOST_REQUIRE_EQUAL(2 * UNIT, owed("alice"_n));
   // 2 over, 1 requested: only the request folds in.
   BOOST_REQUIRE_EQUAL(success(), addkicker(LIQ_ACCOUNT, LIQSOL_SYM, balance - 2 * UNIT, UNIT));
   BOOST_REQUIRE_EQUAL(uint64_t(3 * UNIT), pot());
   // Nothing over the base: nothing folds.
   BOOST_REQUIRE_EQUAL(success(), addkicker(LIQ_ACCOUNT, LIQSOL_SYM, balance, UNIT));
   BOOST_REQUIRE_EQUAL(uint64_t(3 * UNIT), pot());
   BOOST_REQUIRE_EQUAL(3 * UNIT, owed("alice"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Yield in: LIQ_YIELD -> pending -> the swap's reservoir -> clips -> holders
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(queued_yield_sells_through_the_pool_and_pays_holders, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), setkicker(SYSIO_ACCOUNT, 0));
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));

   // Nothing pending: queueing is a no-op, and the pool does not exist yet.
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(10 * UNIT, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(100 * UNIT, supply());   // pending yield is outside supply
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no yield pool registered for this shadow"), queueyield(LIQSOL_SYM));

   // The bootstrap seeds the pool: the LCO liq minted to sysio and deposited
   // with the earmark WIRE, the pair created with the shadow as its yield leg.
   const int64_t treasury_before = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, 1000 * UNIT, 1000 * UNIT));
   BOOST_REQUIRE_EQUAL("LIQPOOL", stat_row()["pair_symbol"].as_string());
   BOOST_REQUIRE_EQUAL(1100 * UNIT, supply());
   BOOST_REQUIRE_EQUAL(0, shadow_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(treasury_before - 1000 * UNIT, wire_balance(SYSIO_ACCOUNT));
   const auto pair = swap_row("stat"_n, "currency_stats", SWAP_ACCOUNT, POOL_SYM.to_symbol_code().value);
   BOOST_REQUIRE_EQUAL(1000 * UNIT, pair["pool1"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(1000 * UNIT, pair["pool2"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(LIQ_ACCOUNT.to_string(), pair["yield_leg"]["contract"].as_string());
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT.to_string(), pair["fee_authority"].as_string());
   BOOST_REQUIRE_EQUAL(0, reservoir());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("the shadow already has a yield pool"),
                       regliqpool(SOLANA, LIQSOL, symbol::from_string("9,POOLB"), UNIT, UNIT));

   // Queueing mints the pending yield to the contract and hands it straight to
   // the reservoir: the contract keeps no balance and accrues nothing.
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));
   BOOST_REQUIRE(pending_row().is_null());
   BOOST_REQUIRE_EQUAL(1110 * UNIT, supply());
   BOOST_REQUIRE_EQUAL(10 * UNIT, reservoir());
   BOOST_REQUIRE_EQUAL(0, shadow_balance(LIQ_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, owed(LIQ_ACCOUNT));
   BOOST_REQUIRE(swap_row("yieldfunds"_n, "fund_receipt", SWAP_ACCOUNT, LIQ_ACCOUNT.to_uint64_t()).is_null());
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));   // nothing left: a no-op
   BOOST_REQUIRE_EQUAL(10 * UNIT, reservoir());

   // A later report pends again; one of another chain is dropped.
   BOOST_REQUIRE_EQUAL(success(), mintyield(ETH, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE(pending_row().is_null());
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(2 * UNIT, pending_row()["quantity"].as<asset>().get_amount());

   // The tick sells a clip of the reservoir through the pool and pays the
   // proceeds in through addyield: the index moves, and alice, a holder, is
   // owed her share of them.
   produce_blocks(40);
   const int64_t alice_before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), tickyield());
   BOOST_REQUIRE_LT(reservoir(), 10 * UNIT);
   BOOST_REQUIRE_LT(0u, pot());
   BOOST_REQUIRE_LT(fc::uint128_t{ 0 }, index());
   const int64_t alice_owed = owed("alice"_n);
   BOOST_REQUIRE_LT(0, alice_owed);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_before + alice_owed, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// Supply plus pending yield never exceeds the asset range: every supply-growing path
// measures its headroom net of what is parked in liqpending, so queueyield can always
// mint it. A report past that headroom is dropped whole, so no value is clipped.
BOOST_FIXTURE_TEST_CASE(pending_yield_is_reserved_against_the_asset_range, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), setkicker(SYSIO_ACCOUNT, 0));
   // The pool first, while the range is open: regliqpool mints the LCO seed. Then one
   // syndication takes supply to 100 base units under the range.
   BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, 1000 * UNIT, 1000 * UNIT));
   const uint64_t range = static_cast<uint64_t>(asset::max_amount);
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, range - 100 - 1000 * UNIT));
   BOOST_REQUIRE_EQUAL(asset::max_amount - 100, supply());

   // 60 fits and parks. A second 60 exceeds the 40 left and is dropped; 40 then fills
   // the range exactly.
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 60));
   BOOST_REQUIRE_EQUAL(60, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 60));
   BOOST_REQUIRE_EQUAL(60, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 40));
   BOOST_REQUIRE_EQUAL(100, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(asset::max_amount - 100, supply());   // pending stays outside supply

   // The reservation binds every other supply-growing path: a mint of one base unit and
   // the governance recredit are both refused.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"), mint("bob"_n, 1));
   BOOST_REQUIRE_EQUAL(0, shadow_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"), recredit("alice"_n, 1));

   // Queueing always fits: the pending yield mints to exactly the range.
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));
   BOOST_REQUIRE(pending_row().is_null());
   BOOST_REQUIRE_EQUAL(asset::max_amount, supply());
   BOOST_REQUIRE_EQUAL(100, reservoir());
   // Nothing can grow the supply now, and a report says why before it is dropped.
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 1));
   BOOST_REQUIRE(pending_row().is_null());
} FC_LOG_AND_RETHROW()

// The other exit: a holder sells shadow into the yield pool for WIRE on the depot, no
// outpost round trip. The pool is an ordinary pair, so alice opens her two deposit rows,
// deposits shadow by transfer, exchanges it for WIRE and withdraws the WIRE to her wallet.
// Both legs conserve exactly: the pool gains what she sold and she receives what the pool
// lost; the shadow supply is untouched, since it moved and nothing burned; the fee and the
// price impact keep her proceeds below par.
BOOST_FIXTURE_TEST_CASE(a_holder_can_sell_shadow_into_the_yield_pool_for_wire, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, 1000 * UNIT, 1000 * UNIT));

   const extended_symbol shadow_ext{ LIQSOL_SYM, LIQ_ACCOUNT };
   const extended_symbol wire_ext{ WIRE_SYM, TOKEN_ACCOUNT };
   BOOST_REQUIRE_EQUAL(success(), openext("alice"_n, "alice"_n, shadow_ext));
   BOOST_REQUIRE_EQUAL(success(), openext("alice"_n, "alice"_n, wire_ext));

   constexpr int64_t sold = 50 * UNIT;
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SWAP_ACCOUNT, sold));
   BOOST_REQUIRE_EQUAL(50 * UNIT, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(1050 * UNIT, shadow_balance(SWAP_ACCOUNT));   // the pool's 1000 plus her deposit

   const auto    before             = swap_pool_row(POOL_SYM);
   const int64_t pool_shadow_before = before["pool1"]["quantity"].as<asset>().get_amount();
   const int64_t pool_wire_before   = before["pool2"]["quantity"].as<asset>().get_amount();
   BOOST_REQUIRE_EQUAL(success(), exchange("alice"_n, POOL_SYM, extended_asset{ asset(sold, LIQSOL_SYM), LIQ_ACCOUNT },
                                           asset(1, WIRE_SYM)));
   const auto    after    = swap_pool_row(POOL_SYM);
   const int64_t received = pool_wire_before - after["pool2"]["quantity"].as<asset>().get_amount();
   BOOST_REQUIRE_EQUAL(pool_shadow_before + sold, after["pool1"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_LT(0, received);
   BOOST_REQUIRE_LT(received, sold);
   BOOST_REQUIRE_EQUAL(1100 * UNIT, supply());

   const int64_t wallet_before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), withdraw("alice"_n, "alice"_n, extended_asset{ asset(received, WIRE_SYM), TOKEN_ACCOUNT }));
   BOOST_REQUIRE_EQUAL(wallet_before + received, wire_balance("alice"_n));
   // The deposit held exactly the proceeds: nothing is left to withdraw.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("insufficient funds"),
                       withdraw("alice"_n, "alice"_n, extended_asset{ asset(1, WIRE_SYM), TOKEN_ACCOUNT }));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(regliqpool_refusals, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), regliqpool(ETH, LIQETH, POOL_SYM, UNIT, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code belongs to another chain"), regliqpool(ETH, LIQSOL, POOL_SYM, UNIT, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("both seeds must be positive"), regliqpool(SOLANA, LIQSOL, POOL_SYM, 0, UNIT));
   BOOST_REQUIRE(mentions(regliqpool(SOLANA, LIQSOL, POOL_SYM, UNIT, UNIT, 30, 0, 86400, 300, 1000, "alice"_n),
                          "missing authority of sysio.liq"));
   BOOST_REQUIRE_EQUAL("", stat_row()["pair_symbol"].as_string());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Governance reconciliation
// ---------------------------------------------------------------------------

// An outpost that refused a de-syndication is reconciled by governance: `recredit` mints the burned
// amount back to the holder, growing the supply by exactly that, and only this contract may sign it.
BOOST_FIXTURE_TEST_CASE(recredit_mints_a_refused_desyndication_back, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 60 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 6 * UNIT));

   BOOST_REQUIRE(mentions(recredit("alice"_n, 10 * UNIT, "alice"_n), "missing authority of sysio.liq"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("quantity must be positive"), recredit("alice"_n, 0));
   BOOST_REQUIRE_EQUAL(success(), recredit("alice"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(70 * UNIT, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(70 * UNIT, supply());
   // The row settled before it grew: the yield earned on the 60 stays, nothing is credited for the 10.
   BOOST_REQUIRE_EQUAL(6 * UNIT, owed("alice"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The emergency stop (sysio.andon)
// ---------------------------------------------------------------------------

// Review focus 2, sysio.liq: while the cord is pulled a transfer INTO a custody contract succeeds and one to
// anyone else is refused; a holder's `claim` and `queueyield` are refused, a custody contract's own claim
// runs, and `mint` runs. With no sysio.andon deployed (every other case here) the cord reads clear. After
// the clear everything refused succeeds.
BOOST_FIXTURE_TEST_CASE(a_freeze_admits_transfers_into_custody_and_refuses_the_rest, sysio_liq_tester) try {
   namespace andon = sysio_system::test_support::andon;
   const auto frozen = wasm_assert_msg(andon::frozen_message);
   create_accounts({ "sysio.bond"_n, "sysio.opreg"_n });
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, UNIT));

   abi_serializer andon_abi_ser;
   andon::deploy(*this, andon_abi_ser, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), andon::pull(*this, andon_abi_ser));

   // In: into sysio.bond, sysio.synd and sysio.opreg.
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "sysio.bond"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SYND_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "sysio.opreg"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(70 * UNIT, shadow_balance("alice"_n));
   // Out: to a holder, and back out of a custody contract to a holder.
   BOOST_REQUIRE_EQUAL(frozen, transfer_shadow("alice"_n, "bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(frozen, transfer_shadow("sysio.bond"_n, "bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(frozen, transfer_shadow("alice"_n, SWAP_ACCOUNT, UNIT));
   BOOST_REQUIRE_EQUAL(0, shadow_balance("bob"_n));
   // A holder's claim and queueyield are refused; a custody contract claims its own row.
   const int64_t alice_owed = owed("alice"_n);
   BOOST_REQUIRE_LT(0, alice_owed);
   BOOST_REQUIRE_EQUAL(frozen, claim("alice"_n));
   BOOST_REQUIRE_EQUAL(frozen, queueyield(LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), claim("sysio.bond"_n));
   // Mint runs.
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(UNIT, shadow_balance("bob"_n));

   BOOST_REQUIRE_EQUAL(success(), andon::clear(*this, andon_abi_ser));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(2 * UNIT, shadow_balance("bob"_n));
   const int64_t alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + alice_owed, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
