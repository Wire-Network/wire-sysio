#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/types/types.pb.h>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"
#include "liq_test_support.hpp"
#include <fc/crypto/private_key.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;
using sysio_system::test_support::codename_mvo;

namespace {

// The contract's `check` messages, spelled as `sysio.bond.cpp` spells them.
constexpr std::string_view bond_increment_msg    = "amount must be a positive multiple of the bond increment";
constexpr std::string_view precision_msg         = "token precision is below the bond increment";
constexpr std::string_view unsupported_token_msg = "unsupported bond token";
constexpr std::string_view statement_len_msg     = "statement exceeds the maximum length";
constexpr std::string_view duplicate_msg         = "a request for this statement already exists";
constexpr std::string_view window_msg            = "window must be positive";
constexpr std::string_view hold_bps_range_msg    = "hold_bps must be positive and at most the basis-point denominator";
constexpr std::string_view amount_positive_msg   = "amount must be positive";
constexpr std::string_view request_missing_msg   = "request not found";
constexpr std::string_view fully_bonded_msg      = "request is fully bonded";
constexpr std::string_view not_accepting_msg     = "request is not accepting bonds";
constexpr std::string_view terminal_msg          = "request is already resolved";
constexpr std::string_view not_bonded_msg        = "request is not bonded";
constexpr std::string_view window_open_msg       = "challenge window has not passed";
constexpr std::string_view hold_state_msg        = "hold is only possible while the request is OPEN or BONDED";
constexpr std::string_view beneficiary_msg       = "hold beneficiary is not an account";
constexpr std::string_view beneficiary_self_msg  = "hold beneficiary cannot be the contract";
constexpr std::string_view nothing_msg           = "nothing to claim";
constexpr std::string_view wire_no_yield_msg     = "WIRE collateral earns no shadow yield";

/// The memo of the one transfer that pays the issuer an INVALID request's bonds.
constexpr std::string_view forfeit_memo = "sysio.bond::forfeit";
/// The memo of the transfer that pays a claim in the request's token.
constexpr std::string_view claim_memo = "sysio.bond::claim";

/// `request_state` names as the ABI renders them.
constexpr std::string_view STATE_OPEN     = "OPEN";
constexpr std::string_view STATE_BONDED   = "BONDED";
constexpr std::string_view STATE_APPROVED = "APPROVED";
constexpr std::string_view STATE_HELD     = "HELD";
constexpr std::string_view STATE_VALID    = "VALID";
constexpr std::string_view STATE_INVALID  = "INVALID";
/// `escrow_kind` names as the ABI renders them, and the key byte of each.
constexpr std::string_view KIND_BOUNTY         = "BOUNTY";
constexpr uint8_t          KIND_BOUNTY_BYTE    = 0;
constexpr std::string_view KIND_HOLD_BOND      = "HOLD_BOND";
constexpr uint8_t          KIND_HOLD_BOND_BYTE = 1;

/// Mirrors the contract's MAX_STATEMENT_BYTES.
constexpr size_t MAX_STATEMENT_BYTES = 1024;

/// Mirrors the contract's PRUNE_RETENTION_SEC: seven days.
constexpr uint32_t PRUNE_RETENTION_SEC = 604'800;

/// `sysio.liq`'s fixed-point scale of the yield index (`YIELD_INDEX_SCALE`).
constexpr uint64_t YIELD_INDEX_SCALE = 1'000'000'000'000;

} // anonymous namespace

/// A shadow-custody `position` as sysio.bond's rows embed it, read raw so the 128-bit checkpoint
/// compares exactly.
struct raw_position {
   fc::uint128_t index_checkpoint = 0;
   uint64_t      owed_wire        = 0;
};
FC_REFLECT(raw_position, (index_checkpoint)(owed_wire))

/// A `bonds` row, raw.
struct raw_bond_row {
   uint64_t     request_id = 0;
   name         underwriter;
   uint64_t     amount = 0;
   raw_position yield;
   bool         paid = false;
};
FC_REFLECT(raw_bond_row, (request_id)(underwriter)(amount)(yield)(paid))

/// An `escrows` row, raw; `kind` is the escrow_kind's byte.
struct raw_escrow_row {
   uint64_t     request_id = 0;
   uint8_t      kind       = 0;
   name         funder;
   uint64_t     amount = 0;
   raw_position yield;
   bool         paid = false;
   name         payee;
   uint64_t     payout = 0;
};
FC_REFLECT(raw_escrow_row, (request_id)(kind)(funder)(amount)(yield)(paid)(payee)(payout))

/// A `sysio.liq` holder row, raw.
struct raw_liq_account {
   asset         balance;
   fc::uint128_t index_checkpoint = 0;
   uint64_t      owed_wire        = 0;
};
FC_REFLECT(raw_liq_account, (balance)(index_checkpoint)(owed_wire))

/// A `sysio.liq` `yieldidx` row, raw.
struct raw_yield_index {
   fc::uint128_t index = 0;
   uint64_t      pot   = 0;
   uint64_t      carry = 0;
};
FC_REFLECT(raw_yield_index, (index)(pot)(carry))

/// sysio.bond on the depot: the registries and custody contracts it resolves tokens through,
/// sysio.liq as the shadow custody contract and yield source, with governance funding fixture balances. The treasury supply of WIRE sits on `sysio`, as the bootstrap leaves it.
class sysio_bond_tester : public tester {
public:
   static constexpr auto BOND_ACCOUNT   = "sysio.bond"_n;
   static constexpr auto LIQ_ACCOUNT    = "sysio.liq"_n;
   static constexpr auto SYND_ACCOUNT   = "sysio.synd"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto SYSIO_ACCOUNT  = "sysio"_n;

   static inline const symbol WIRE_SYM   = symbol::from_string("9,WIRE");
   static inline const symbol LIQSOL_SYM = symbol::from_string("9,LIQSOL");
   static constexpr uint64_t  UNIT       = 1'000'000'000;   // one whole token of a precision-9 symbol

   static constexpr std::string_view SOLANA = "SOLANA";
   static constexpr std::string_view LIQSOL = "LIQSOL";
   static constexpr std::string_view WIRE   = "WIRE";

   /// A challenge window of three hours.
   static constexpr uint32_t WINDOW_SEC = 10800;

   sysio_bond_tester() {
      produce_blocks(2);
      create_accounts({ BOND_ACCOUNT, LIQ_ACCOUNT, SYND_ACCOUNT, TOKEN_ACCOUNT, TOKENS_ACCOUNT, CHAINS_ACCOUNT,
                        "issuer"_n, "alice"_n, "bob"_n, "carol"_n });
      produce_blocks(2);

      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi_ser, true);
      deploy(TOKENS_ACCOUNT, contracts::tokens_wasm(), contracts::tokens_abi(), tokens_abi_ser, true);
      deploy(TOKEN_ACCOUNT,  contracts::token_wasm(),  contracts::token_abi(),  token_abi_ser,  true);
      deploy(LIQ_ACCOUNT,    contracts::liq_wasm(),    contracts::liq_abi(),    liq_abi_ser,    true);
      deploy(BOND_ACCOUNT,   contracts::bond_wasm(),   contracts::bond_abi(),   bond_abi_ser,   true);

      // WIRE: the treasury supply on sysio, and working balances for the issuer and underwriters.
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, TOKEN_ACCOUNT, "create"_n, mvo()
         ("issuer", SYSIO_ACCOUNT)("maximum_supply", asset(1'000'000'000 * UNIT, WIRE_SYM))));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, SYSIO_ACCOUNT, "issue"_n, mvo()
         ("to", SYSIO_ACCOUNT)("quantity", asset(1'000'000'000 * UNIT, WIRE_SYM))("memo", "")));
      for (auto funder : { "issuer"_n, "alice"_n, "bob"_n, "carol"_n })
         BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, funder, 1'000'000 * UNIT));

      // The SOLANA outpost, its liq token and that token's shadow on the depot.
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, SOLANA, 2));
      BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, LIQSOL, 9));
      BOOST_REQUIRE_EQUAL(success(), create_shadow(LIQSOL_SYM, LIQSOL));

      // Shadow LIQSOL for the issuer and two underwriters, funded through governance credit.
      for (auto holder : { "alice"_n, "bob"_n, "issuer"_n }) {
         BOOST_REQUIRE_EQUAL(success(), mint(holder, 1'000 * UNIT));
         BOOST_REQUIRE_EQUAL(1'000 * UNIT, liq_balance(holder));
      }
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

   /// Push one action in its own block, keeping its trace in `last_trace` (null after a failure).
   action_result push(name code, abi_serializer& ser, name signer, name action_name, const variant_object& data) {
      return sysio_system::test_support::push_contract_action_and_produce_block(*this, code, ser, signer,
                                                                                action_name, data, last_trace);
   }
   action_result push_bond(name signer, name action_name, const variant_object& data) {
      return push(BOND_ACCOUNT, bond_abi_ser, signer, action_name, data);
   }

   // --- registries and sysio.liq ---

   // Registrations inside the epoch-0 bootstrap window land ACTIVE, as the launch bootstrap's do.
   action_result regchain(ChainKind kind, std::string_view code, uint32_t external_chain_id) {
      return push(CHAINS_ACCOUNT, chains_abi_ser, CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", kind)("code", codename_mvo(code))("external_chain_id", external_chain_id)
         ("name", std::string("outpost"))("description", std::string{})
         ("outpost", sysio_system::test_support::no_outpost_mvo()));
   }
   /// Register a token of the SOLANA outpost with `precision` decimals and bind it to the chain.
   action_result regtoken(TokenKind kind, std::string_view code, uint32_t precision) {
      const std::vector<char> address(32, char(0x5a));
      auto r = push(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT, "regtoken"_n, mvo()
         ("kind", kind)("code", codename_mvo(code))("symbol_name", std::string(code))("description", std::string{})
         ("precision", precision)("address", mvo()("kind", ChainKind::CHAIN_KIND_SVM)("address", address)));
      if (r != success()) return r;
      return push(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT, "regctok"_n, mvo()
         ("chain_code", codename_mvo(SOLANA))("token_code", codename_mvo(code))("contract_addr", address)
         ("is_native", false));
   }
   /// Register `sym` on sysio.liq as the shadow of the SOLANA liq token `token_code`.
   action_result create_shadow(symbol sym, std::string_view token_code) {
      return push(LIQ_ACCOUNT, liq_abi_ser, LIQ_ACCOUNT, "create"_n, mvo()
         ("sym", sym)("chain_code", codename_mvo(SOLANA))("token_code", codename_mvo(token_code)));
   }
   /// Seed a nine-decimal fixture token through the ledger's governance credit action.
   action_result mint(name account, uint64_t amount, std::string_view token_code = LIQSOL) {
      const symbol sym{LIQSOL_SYM.decimals(), std::string(token_code)};
      return push(LIQ_ACCOUNT, liq_abi_ser, LIQ_ACCOUNT, "recredit"_n, mvo()
         ("holder", account)("quantity", asset(static_cast<int64_t>(amount), sym)));
   }
   /// Distribute `amount` WIRE from `from` to the holders of `sym` (LIQSOL by default) through
   /// sysio.liq, raising its index.
   action_result addyield(name from, uint64_t amount, symbol sym = LIQSOL_SYM) {
      return push(LIQ_ACCOUNT, liq_abi_ser, from, "addyield"_n, mvo()
         ("from", from)("quantity", asset(static_cast<int64_t>(amount), WIRE_SYM))
         ("target", sym.to_symbol_code()));
   }
   /// Burn the fixture holder's shadow through the ledger's desyndication action.
   /// A trusted SVM link supplies the outbound destination for the supply-drain case.
   action_result burn_shadow(name holder, uint64_t amount, symbol sym, std::string_view token_code) {
      constexpr auto authex_account = "sysio.authex"_n;
      constexpr auto msgch_account = "sysio.msgch"_n;
      for (auto account : {authex_account, msgch_account})
         if (control->find_account(account) == nullptr) create_accounts({account});
      abi_serializer authex_ser, msgch_ser;
      deploy(authex_account, contracts::authex_wasm(), contracts::authex_abi(), authex_ser, true);
      deploy(msgch_account, contracts::msgch_wasm(), contracts::msgch_abi(), msgch_ser, true);
      const auto pubkey = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed).get_public_key();
      auto result = push(authex_account, authex_ser, authex_account, "recordlink"_n, mvo()
         ("account", holder)("chain_kind", ChainKind::CHAIN_KIND_SVM)("pub_key", pubkey)
         ("native_address", sysio_liq::test_support::native_address_of(pubkey)));
      if (result != success()) return result;
      return push(LIQ_ACCOUNT, liq_abi_ser, holder, "desyndicate"_n, mvo()
         ("holder", holder)("quantity", asset(static_cast<int64_t>(amount), sym)));
   }
   /// sysio.liq pays `holder` the WIRE its LIQSOL row is owed.
   action_result liq_claim(name holder) {
      return push(LIQ_ACCOUNT, liq_abi_ser, holder, "claim"_n, mvo()("holder", holder)
         ("sym", LIQSOL_SYM.to_symbol_code()));
   }
   action_result transfer_wire(name from, name to, uint64_t amount) {
      return push(TOKEN_ACCOUNT, token_abi_ser, from, "transfer"_n, mvo()
         ("from", from)("to", to)("quantity", asset(static_cast<int64_t>(amount), WIRE_SYM))("memo", ""));
   }

   // --- sysio.bond actions ---

   action_result request(name issuer, name schema, const std::vector<char>& statement, std::string_view token_code,
                         uint64_t covered, uint64_t bounty, uint32_t window_sec = WINDOW_SEC) {
      return push_bond(issuer, "request"_n, mvo()
         ("issuer", issuer)("schema", schema)("statement", statement)("token_code", codename_mvo(token_code))
         ("covered", covered)("bounty", bounty)("window_sec", window_sec));
   }
   action_result addbounty(name signer, uint64_t request_id, uint64_t amount) {
      return push_bond(signer, "addbounty"_n, mvo()("request_id", request_id)("amount", amount));
   }
   action_result setconfig(name signer, uint32_t hold_bps) {
      return push_bond(signer, "setconfig"_n, mvo()("hold_bps", hold_bps));
   }
   action_result accept(name underwriter, uint64_t request_id, uint64_t amount) {
      return push_bond(underwriter, "accept"_n, mvo()("underwriter", underwriter)("request_id", request_id)
         ("amount", amount));
   }
   action_result hold(name signer, uint64_t request_id, name beneficiary) {
      return push_bond(signer, "hold"_n, mvo()("request_id", request_id)("beneficiary", beneficiary));
   }
   /// `approve` is permissionless; `carol`, a bystander, signs it.
   action_result approve(uint64_t request_id) {
      return push_bond("carol"_n, "approve"_n, mvo()("request_id", request_id));
   }
   action_result rslvvalid(name signer, uint64_t request_id) {
      return push_bond(signer, "rslvvalid"_n, mvo()("request_id", request_id));
   }
   action_result rslvinvalid(name signer, uint64_t request_id) {
      return push_bond(signer, "rslvinvalid"_n, mvo()("request_id", request_id));
   }
   /// `claim` is permissionless; `signer` defaults to the account paid.
   action_result claim(uint64_t request_id, name account, std::optional<name> signer = std::nullopt) {
      return push_bond(signer.value_or(account), "claim"_n, mvo()("request_id", request_id)("account", account));
   }
   /// `sweepyield` is permissionless; `carol` signs it.
   action_result sweepyield(std::string_view token_code) {
      return push_bond("carol"_n, "sweepyield"_n, mvo()("token_code", codename_mvo(token_code)));
   }
   /// `prune` is permissionless; `carol` signs it.
   action_result prune(uint64_t from_id, uint32_t limit) {
      return push_bond("carol"_n, "prune"_n, mvo()("from_id", from_id)("limit", limit));
   }
   /// Request `statement_fill`'s statement in LIQSOL, covering 10 UNIT with a window of `window_sec`, and
   /// have alice bond all of it: request `request_id` is BONDED.
   void bonded_request(uint64_t request_id, char statement_fill, uint32_t window_sec = WINDOW_SEC) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, statement_fill), LIQSOL,
                                             10 * UNIT, 0, window_sec));
      BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, request_id, 10 * UNIT));
      BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(request_id));
   }
   /// Let `window_sec` and one more second pass on the chain clock.
   void pass_window(uint32_t window_sec) { produce_block(fc::seconds(window_sec + 1)); }
   /// Let the retention period `prune` keeps a ruled request for pass on the chain clock.
   void pass_retention() { produce_block(fc::seconds(PRUNE_RETENTION_SEC)); }

   static std::vector<char> bytes(size_t size, char fill) { return std::vector<char>(size, fill); }

   // --- rows ---

   fc::variant decode(abi_serializer& ser, const char* type, const std::vector<char>& data) {
      return data.empty()
                ? fc::variant()
                : ser.binary_to_variant(type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   fc::variant request_row_of(uint64_t id) {
      return decode(bond_abi_ser, "request_row", get_row_by_id(BOND_ACCOUNT, BOND_ACCOUNT, "requests"_n, id));
   }
   std::string request_state_of(uint64_t id) { return request_row_of(id)["state"].as_string(); }
   /// Append `value` to a kv key as the big-endian word the CDT key serializer writes.
   static void append_key_word(std::string& key, uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8) key.push_back(char((value >> shift) & 0xff));
   }
   /// sysio.bond's kv rows of `table` in key order whose key starts with `prefix`, as raw values.
   std::vector<std::vector<char>> kv_rows(name table, const std::string& prefix) {
      const auto&                    kv_idx   = control->db().get_index<kv_index, by_code_key>();
      const auto                     table_id = compute_table_id(table.to_uint64_t());
      std::vector<std::vector<char>> rows;
      for (auto itr = kv_idx.lower_bound(boost::make_tuple(BOND_ACCOUNT, table_id, std::string_view(prefix)));
           itr != kv_idx.end() && itr->code == BOND_ACCOUNT && itr->table_id == table_id &&
           itr->key_view().starts_with(prefix);
           ++itr)
         rows.emplace_back(itr->value.data(), itr->value.data() + itr->value.size());
      return rows;
   }
   /// The kv row of sysio.bond's `table` whose key is exactly `key`; null when there is none.
   const kv_object* kv_row(name table, const std::string& key) {
      const auto& kv_idx = control->db().get_index<kv_index, by_code_key>();
      const auto  it     = kv_idx.find(
         boost::make_tuple(BOND_ACCOUNT, compute_table_id(table.to_uint64_t()), std::string_view(key)));
      return it == kv_idx.end() ? nullptr : &*it;
   }
   /// The kv key of request `id`: the id as a big-endian word.
   static std::string request_key_of(uint64_t id) {
      std::string key;
      append_key_word(key, id);
      return key;
   }
   /// The account that pays the RAM of the kv row of `table` keyed `key`; the test fails without one.
   name payer_of(name table, const std::string& key) {
      const auto* row = kv_row(table, key);
      BOOST_REQUIRE_MESSAGE(row != nullptr, "no " << table << " row");
      return row->payer;
   }
   /// The RAM `account` uses, in bytes.
   int64_t ram_usage(name account) { return control->get_resource_limits_manager().get_account_ram_usage(account); }
   /// Rewrite request `id`'s stored token code to `token_code`, bypassing the contract, to stand for a
   /// token that no longer resolves.
   void rewrite_request_token(uint64_t id, std::string_view token_code) {
      mvo row(request_row_of(id).get_object());
      row("token_code", codename_mvo(token_code));
      const auto bytes = bond_abi_ser.variant_to_binary("request_row", row,
                                                        abi_serializer::create_yield_function(abi_serializer_max_time));
      const auto* obj = kv_row("requests"_n, request_key_of(id));
      BOOST_REQUIRE(obj != nullptr);
      auto& db = const_cast<chainbase::database&>(control->db());
      db.modify(*obj, [&](auto& r) { r.value.assign(bytes.data(), bytes.size()); });
   }
   /// The `escrows` row of `request_id` and `kind`: its kv key is the request id as a big-endian
   /// word followed by the kind byte.
   fc::variant escrow_row_of(uint64_t request_id, uint8_t kind) {
      std::string key;
      append_key_word(key, request_id);
      key.push_back(char(kind));
      const auto rows = kv_rows("escrows"_n, key);
      return rows.empty() ? fc::variant() : decode(bond_abi_ser, "escrow_row", rows.front());
   }
   /// The `bonds` rows of `request_id`: their kv keys start with the request id as a big-endian word.
   std::vector<fc::variant> bond_rows_of(uint64_t request_id) {
      std::string prefix;
      append_key_word(prefix, request_id);
      std::vector<fc::variant> rows;
      for (const auto& value : kv_rows("bonds"_n, prefix)) rows.push_back(decode(bond_abi_ser, "bond_row", value));
      return rows;
   }
   /// The amount `underwriter` has bonded on `request_id`; 0 with no bond row.
   uint64_t bond_amount_of(uint64_t request_id, name underwriter) {
      for (const auto& row : bond_rows_of(request_id))
         if (row["underwriter"].as<name>() == underwriter) return row["amount"].as_uint64();
      return 0;
   }
   fc::variant config_row() {
      return decode(bond_abi_ser, "bond_config",
                    get_row_by_account(BOND_ACCOUNT, BOND_ACCOUNT, "bondconfig"_n, "bondconfig"_n));
   }
   uint64_t wire_balance(name holder) {
      const auto row = decode(token_abi_ser, "account",
                              get_row_by_id(TOKEN_ACCOUNT, holder, "accounts"_n, WIRE_SYM.to_symbol_code().value));
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }
   uint64_t liq_balance(name holder, symbol sym = LIQSOL_SYM) {
      const auto row = decode(liq_abi_ser, "account",
                              get_row_by_id(LIQ_ACCOUNT, holder, "accounts"_n, sym.to_symbol_code().value));
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }

   /// The `bonds` row of `request_id` and `underwriter`, raw; the test fails without one.
   raw_bond_row raw_bond_of(uint64_t request_id, name underwriter) {
      std::string key;
      append_key_word(key, request_id);
      append_key_word(key, underwriter.to_uint64_t());
      const auto rows = kv_rows("bonds"_n, key);
      BOOST_REQUIRE_MESSAGE(!rows.empty(), "no bond row for " << request_id << " " << underwriter);
      return fc::raw::unpack<raw_bond_row>(rows.front());
   }
   /// The `escrows` row of `request_id` and `kind`, raw; the test fails without one.
   raw_escrow_row raw_escrow_of(uint64_t request_id, uint8_t kind) {
      std::string key;
      append_key_word(key, request_id);
      key.push_back(char(kind));
      const auto rows = kv_rows("escrows"_n, key);
      BOOST_REQUIRE_MESSAGE(!rows.empty(), "no escrow row for " << request_id << " " << int(kind));
      return fc::raw::unpack<raw_escrow_row>(rows.front());
   }
   /// sysio.liq's cumulative yield index of LIQSOL; 0 before its first distribution.
   fc::uint128_t liq_index() {
      const auto data = get_row_by_id(LIQ_ACCOUNT, LIQ_ACCOUNT, "yieldidx"_n, LIQSOL_SYM.to_symbol_code().value);
      return data.empty() ? fc::uint128_t{0} : fc::raw::unpack<raw_yield_index>(data).index;
   }
   /// The WIRE `holder`'s LIQSOL row is owed now: `sysio.liq`'s own formula over the live index, what
   /// `custody::custodian_owed` reports for a custodian.
   uint64_t liq_owed(name holder) {
      const auto data = get_row_by_id(LIQ_ACCOUNT, holder, "accounts"_n, LIQSOL_SYM.to_symbol_code().value);
      if (data.empty()) return 0;
      const auto row = fc::raw::unpack<raw_liq_account>(data);
      if (row.balance.get_amount() <= 0) return row.owed_wire;
      return static_cast<uint64_t>(row.owed_wire + static_cast<fc::uint128_t>(row.balance.get_amount()) *
                                                      (liq_index() - row.index_checkpoint) / YIELD_INDEX_SCALE);
   }
   /// The `yieldpool` row of `token_code` (LIQSOL by default) as the ABI renders it; null before its
   /// first pull.
   fc::variant pool_row_of(std::string_view token_code = LIQSOL) {
      return decode(bond_abi_ser, "pool_row",
                    get_row_by_id(BOND_ACCOUNT, BOND_ACCOUNT, "yieldpool"_n, fc::slug_name{token_code}.value));
   }

   // --- the last pushed transaction's trace ---

   /// True iff the last pushed transaction delivered sysio.bond's `action_name` to `receiver`: the
   /// notification copy `require_recipient(receiver)` makes.
   bool notified(name receiver, name action_name) const {
      if (!last_trace) return false;
      for (const auto& at : last_trace->action_traces)
         if (at.act.account == BOND_ACCOUNT && at.act.name == action_name && at.receiver == receiver) return true;
      return false;
   }
   /// One `transfer` the last pushed transaction executed.
   struct seen_transfer {
      name        from;
      name        to;
      asset       quantity;
      std::string memo;
   };
   /// Every `transfer` the last pushed transaction executed on `token_contract` (the contract's own
   /// execution, not the notification copies), in execution order.
   std::vector<seen_transfer> transfers_on(name token_contract, abi_serializer& ser) {
      std::vector<seen_transfer> seen;
      if (!last_trace) return seen;
      for (const auto& at : last_trace->action_traces) {
         if (at.act.account != token_contract || at.act.name != "transfer"_n || at.receiver != token_contract) continue;
         const auto v = ser.binary_to_variant("transfer", at.act.data,
                                              abi_serializer::create_yield_function(abi_serializer_max_time));
         seen.push_back({v["from"].as<name>(), v["to"].as<name>(), v["quantity"].as<asset>(), v["memo"].as_string()});
      }
      return seen;
   }
   std::vector<seen_transfer> settlements() {
      std::vector<seen_transfer> seen;
      if (!last_trace) return seen;
      for (const auto& at : last_trace->action_traces) {
         if (at.act.account != LIQ_ACCOUNT || at.act.name != "settle"_n || at.receiver != LIQ_ACCOUNT) continue;
         const auto v = liq_abi_ser.binary_to_variant("settle", at.act.data,
                                              abi_serializer::create_yield_function(abi_serializer_max_time));
         seen.push_back({v["custodian"].as<name>(), v["beneficiary"].as<name>(), v["quantity"].as<asset>(), {}});
      }
      return seen;
   }
   action_result claimwire(name account) {
      return push_bond(account, "claimwire"_n, mvo()("account", account));
   }
   /// The `sysio.liq::addyield` actions the last pushed transaction executed, as `(from, quantity)`.
   std::vector<std::pair<name, asset>> addyields() {
      std::vector<std::pair<name, asset>> seen;
      if (!last_trace) return seen;
      for (const auto& at : last_trace->action_traces) {
         if (at.act.account != LIQ_ACCOUNT || at.act.name != "addyield"_n || at.receiver != LIQ_ACCOUNT) continue;
         const auto v = liq_abi_ser.binary_to_variant("addyield", at.act.data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
         seen.emplace_back(v["from"].as<name>(), v["quantity"].as<asset>());
      }
      return seen;
   }

   abi_serializer bond_abi_ser, liq_abi_ser, token_abi_ser, tokens_abi_ser, chains_abi_ser;
   transaction_trace_ptr last_trace;   ///< trace of the last successful push; null after a failure
};

BOOST_AUTO_TEST_SUITE(sysio_bond_tests)

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(config_defaults_to_ten_percent_and_sysio_sets_it, sysio_bond_tester) try {
   BOOST_REQUIRE(config_row().is_null());   // no row: `hold` reads DEFAULT_HOLD_BPS

   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), setconfig("alice"_n, 500));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(hold_bps_range_msg)), setconfig(SYSIO_ACCOUNT, 10001));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(hold_bps_range_msg)), setconfig(SYSIO_ACCOUNT, 0));
   BOOST_REQUIRE(config_row().is_null());

   BOOST_REQUIRE_EQUAL(success(), setconfig(SYSIO_ACCOUNT, 500));
   BOOST_REQUIRE_EQUAL(500u, config_row()["hold_bps"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SYSIO_ACCOUNT, 10000));
   BOOST_REQUIRE_EQUAL(10000u, config_row()["hold_bps"].as_uint64());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// request
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(request_in_shadow_liq_escrows_the_bounty, sysio_bond_tester) try {
   const auto before      = liq_balance("issuer"_n);
   const auto bond_before = liq_balance(BOND_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 1 * UNIT, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(bond_before + 1 * UNIT, liq_balance(BOND_ACCOUNT));

   const auto row = request_row_of(1);
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE_EQUAL("issuer", row["issuer"].as_string());
   BOOST_REQUIRE_EQUAL("oppenvelope", row["schema"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, row["covered"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, row["bonded"].as_uint64());
   BOOST_REQUIRE_EQUAL(1 * UNIT, row["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(WINDOW_SEC, row["window_sec"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, row["hold_bond"].as_uint64());

   const auto escrow = escrow_row_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE(!escrow.is_null());
   BOOST_REQUIRE_EQUAL(KIND_BOUNTY, escrow["kind"].as_string());
   BOOST_REQUIRE_EQUAL("issuer", escrow["funder"].as_string());
   BOOST_REQUIRE_EQUAL(1 * UNIT, escrow["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(false, escrow["paid"].as_bool());

   // Ids come from the counter: the next request is 2.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE(!request_row_of(2).is_null());
   BOOST_REQUIRE(escrow_row_of(2, KIND_BOUNTY_BYTE).is_null());   // no bounty, no escrow
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(request_in_wire_is_accepted, sysio_bond_tester) try {
   const auto before = wire_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(before, wire_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());

   // A WIRE bounty is pulled from sysio.token.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), WIRE, 10 * UNIT, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 3 * UNIT, wire_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(3 * UNIT, wire_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(3 * UNIT, escrow_row_of(2, KIND_BOUNTY_BYTE)["amount"].as_uint64());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(request_refuses_a_covered_amount_off_the_increment, sysio_bond_tester) try {
   // Precision 9 with two bond decimals: the increment is 10'000'000 base units.
   const uint64_t increment = 10'000'000;
   const auto     refused   = wasm_assert_msg(std::string(bond_increment_msg));
   BOOST_REQUIRE_EQUAL(refused, request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT + 1, 0));
   BOOST_REQUIRE_EQUAL(refused, request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, increment - 1, 0));
   BOOST_REQUIRE_EQUAL(refused, request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 0, 0));
   BOOST_REQUIRE(request_row_of(1).is_null());

   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, increment, 0));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(request_refuses_a_token_below_two_decimals, sysio_bond_tester) try {
   constexpr std::string_view LIQX = "LIQX";
   BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, LIQX, 1));
   BOOST_REQUIRE_EQUAL(success(), create_shadow(symbol::from_string("1,LIQX"), LIQX));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(precision_msg)),
                       request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQX, 100, 0));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(request_refuses_an_unknown_token_and_a_long_statement, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(unsupported_token_msg)),
                       request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), "NOPE", 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(statement_len_msg)),
                       request("issuer"_n, "oppenvelope"_n, bytes(MAX_STATEMENT_BYTES + 1, 'a'), LIQSOL,
                               10 * UNIT, 0));
   BOOST_REQUIRE(request_row_of(1).is_null());

   // Exactly the maximum is accepted.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(MAX_STATEMENT_BYTES, 'a'), LIQSOL,
                                          10 * UNIT, 0));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(request_refuses_a_zero_window_and_another_signer, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(window_msg)),
                       request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0, 0));
   BOOST_REQUIRE_EQUAL(error("missing authority of issuer"),
                       push_bond("alice"_n, "request"_n, mvo()
                          ("issuer", "issuer"_n)("schema", "oppenvelope"_n)("statement", bytes(32, 'a'))
                          ("token_code", codename_mvo(LIQSOL))("covered", 10 * UNIT)("bounty", 0)
                          ("window_sec", WINDOW_SEC)));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(request_refuses_a_duplicate_statement_for_the_same_issuer, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(duplicate_msg)),
                       request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 20 * UNIT, 0));
   // The digest covers the token nowhere: a different token is still the same statement.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(duplicate_msg)),
                       request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 0));

   // Another issuer, or another schema, is another statement.
   BOOST_REQUIRE_EQUAL(success(), request("alice"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "otherschema"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL("alice", request_row_of(2)["issuer"].as_string());
   BOOST_REQUIRE_EQUAL("otherschema", request_row_of(3)["schema"].as_string());
} FC_LOG_AND_RETHROW()

// A request's rows bill the issuer that created them, never the sysio pool: only the id counter,
// written once, is the contract's own.
BOOST_FIXTURE_TEST_CASE(request_rows_bill_the_issuer, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("requests"_n, request_key_of(1)));
   std::string bounty_key = request_key_of(1);
   bounty_key.push_back(char(KIND_BOUNTY_BYTE));
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("escrows"_n, bounty_key));

   // With the counter in place, a second request adds nothing to sysio's RAM and grows the issuer's.
   const auto sysio_ram  = ram_usage(SYSIO_ACCOUNT);
   const auto issuer_ram = ram_usage("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(sysio_ram, ram_usage(SYSIO_ACCOUNT));
   BOOST_REQUIRE_GT(ram_usage("issuer"_n), issuer_ram);

   // A raise by the issuer keeps the rows its own.
   BOOST_REQUIRE_EQUAL(success(), addbounty("issuer"_n, 2, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(sysio_ram, ram_usage(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("requests"_n, request_key_of(2)));
} FC_LOG_AND_RETHROW()

// A privileged account -- a system contract, as sysio.synd is when it issues for its envelopes -- has
// no RAM quota of its own, so the rows of its request bill the sysio RAM pool instead.
BOOST_FIXTURE_TEST_CASE(a_privileged_issuer_bills_sysio, sysio_bond_tester) try {
   // sysio.tokens, the token registry, is a privileged system contract of the fixture.
   constexpr auto system_contract = TOKENS_ACCOUNT;
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, system_contract, 100 * UNIT));
   const auto issuer_ram  = ram_usage(system_contract);
   const auto sysio_ram = ram_usage(SYSIO_ACCOUNT);

   BOOST_REQUIRE_EQUAL(success(),
                       request(system_contract, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("requests"_n, request_key_of(1)));
   std::string bounty_key = request_key_of(1);
   bounty_key.push_back(char(KIND_BOUNTY_BYTE));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("escrows"_n, bounty_key));
   BOOST_REQUIRE_EQUAL(issuer_ram, ram_usage(system_contract));
   BOOST_REQUIRE_GT(ram_usage(SYSIO_ACCOUNT), sysio_ram);

   // A raise keeps the rows on the pool.
   BOOST_REQUIRE_EQUAL(success(), addbounty(system_contract, 1, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("escrows"_n, bounty_key));
   BOOST_REQUIRE_EQUAL(issuer_ram, ram_usage(system_contract));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// addbounty
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(addbounty_raises_and_escrows, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(success(), addbounty("issuer"_n, 1, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 2 * UNIT, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(3 * UNIT, liq_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(3 * UNIT, request_row_of(1)["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(3 * UNIT, escrow_row_of(1, KIND_BOUNTY_BYTE)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));

   BOOST_REQUIRE_EQUAL(error("missing authority of issuer"), addbounty("alice"_n, 1, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(amount_positive_msg)), addbounty("issuer"_n, 1, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), addbounty("issuer"_n, 99, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(3 * UNIT, request_row_of(1)["bounty"].as_uint64());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(addbounty_opens_the_escrow_of_a_request_without_one, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 0));
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());
   const auto before = wire_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(success(), addbounty("issuer"_n, 1, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 5 * UNIT, wire_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(5 * UNIT, request_row_of(1)["bounty"].as_uint64());
   const auto escrow = escrow_row_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE_EQUAL("issuer", escrow["funder"].as_string());
   BOOST_REQUIRE_EQUAL(5 * UNIT, escrow["amount"].as_uint64());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// accept
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(accept_pulls_the_bond_and_records_it, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before      = liq_balance("alice"_n);
   const auto bond_before = liq_balance(BOND_ACCOUNT);

   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 4 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(bond_before + 4 * UNIT, liq_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(4 * UNIT, bond_amount_of(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(4 * UNIT, request_row_of(1)["bonded"].as_uint64());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE(request_row_of(1)["bonded_at"].as<time_point>() == time_point{});

   const auto rows = bond_rows_of(1);
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   BOOST_REQUIRE_EQUAL(false, rows.front()["paid"].as_bool());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_refuses_off_increment_and_zero, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before  = liq_balance("alice"_n);
   const auto refused = wasm_assert_msg(std::string(bond_increment_msg));

   BOOST_REQUIRE_EQUAL(refused, accept("alice"_n, 1, 4 * UNIT + 1));
   BOOST_REQUIRE_EQUAL(refused, accept("alice"_n, 1, 0));
   BOOST_REQUIRE_EQUAL(before, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, request_row_of(1)["bonded"].as_uint64());
   BOOST_REQUIRE(bond_rows_of(1).empty());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_is_reduced_to_the_remainder_and_pulls_only_that, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE(!notified("issuer"_n, "accept"_n));
   const auto before      = liq_balance("bob"_n);
   const auto bond_before = liq_balance(BOND_ACCOUNT);

   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 100 * UNIT));
   BOOST_REQUIRE(!notified("issuer"_n, "accept"_n));   // OPEN -> BONDED notifies no one
   BOOST_REQUIRE_EQUAL(before - 6 * UNIT, liq_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(bond_before + 6 * UNIT, liq_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(6 * UNIT, bond_amount_of(1, "bob"_n));

   const auto row = request_row_of(1);
   BOOST_REQUIRE_EQUAL(10 * UNIT, row["bonded"].as_uint64());
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));
   BOOST_REQUIRE(row["bonded_at"].as<time_point>() != time_point{});
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_refuses_once_fully_bonded, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));
   const auto bonded_at = request_row_of(1)["bonded_at"].as<time_point>();
   const auto before    = liq_balance("bob"_n);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(fully_bonded_msg)), accept("bob"_n, 1, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(fully_bonded_msg)), accept("alice"_n, 1, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(before, liq_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(0u, bond_amount_of(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, bond_amount_of(1, "alice"_n));
   BOOST_REQUIRE(bonded_at == request_row_of(1)["bonded_at"].as<time_point>());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_twice_by_one_underwriter_adds_to_one_row, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before = liq_balance("alice"_n);

   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 5 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(1u, bond_rows_of(1).size());
   BOOST_REQUIRE_EQUAL(5 * UNIT, bond_amount_of(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(5 * UNIT, request_row_of(1)["bonded"].as_uint64());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
} FC_LOG_AND_RETHROW()

// The positions of accept and request checkpoint sysio.liq's live index, once a distribution has
// raised it above zero, and a second accept banks what the first bond earned in between.
BOOST_FIXTURE_TEST_CASE(accept_and_request_checkpoint_the_live_index, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   const auto first_index = liq_index();
   BOOST_REQUIRE(first_index > 0);

   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   const auto bounty = raw_escrow_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE(bounty.yield.index_checkpoint == first_index);
   BOOST_REQUIRE_EQUAL(0u, bounty.yield.owed_wire);

   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 2 * UNIT));
   auto bond = raw_bond_of(1, "alice"_n);
   BOOST_REQUIRE(bond.yield.index_checkpoint == first_index);
   BOOST_REQUIRE_EQUAL(0u, bond.yield.owed_wire);

   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   const auto second_index = liq_index();
   BOOST_REQUIRE(second_index > first_index);
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 3 * UNIT));
   bond = raw_bond_of(1, "alice"_n);
   BOOST_REQUIRE(bond.yield.index_checkpoint == second_index);
   // The 2 UNIT bonded between the two distributions earned their share of the second one.
   const auto earned = static_cast<uint64_t>(static_cast<fc::uint128_t>(2 * UNIT) * (second_index - first_index) /
                                             YIELD_INDEX_SCALE);
   BOOST_REQUIRE(earned > 0);
   BOOST_REQUIRE_EQUAL(earned, bond.yield.owed_wire);
   BOOST_REQUIRE_EQUAL(5 * UNIT, bond.amount);

   // A WIRE bond earns no shadow yield: its position stays default.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), WIRE, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 2, 4 * UNIT));
   const auto wire_bond = raw_bond_of(2, "alice"_n);
   BOOST_REQUIRE(wire_bond.yield.index_checkpoint == 0);
   BOOST_REQUIRE_EQUAL(0u, wire_bond.yield.owed_wire);
} FC_LOG_AND_RETHROW()

// A bond row bills its underwriter; the request row the accept updates stays the issuer's.
BOOST_FIXTURE_TEST_CASE(accept_bills_the_underwriter, sysio_bond_tester) try {
   // The bounty opens sysio.bond's own LIQSOL holder row on sysio.liq first, which sysio.liq bills.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   const auto sysio_ram  = ram_usage(SYSIO_ACCOUNT);
   const auto issuer_ram = ram_usage("issuer"_n);
   const auto alice_ram  = ram_usage("alice"_n);

   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   std::string bond_key = request_key_of(1);
   append_key_word(bond_key, "alice"_n.to_uint64_t());
   BOOST_REQUIRE_EQUAL("alice"_n, payer_of("bonds"_n, bond_key));
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("requests"_n, request_key_of(1)));
   BOOST_REQUIRE_GT(ram_usage("alice"_n), alice_ram);
   BOOST_REQUIRE_EQUAL(issuer_ram, ram_usage("issuer"_n));
   BOOST_REQUIRE_EQUAL(sysio_ram, ram_usage(SYSIO_ACCOUNT));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_in_wire_pulls_wire, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 0));
   const auto before     = wire_balance("alice"_n);
   const auto liq_before = liq_balance("alice"_n);

   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(before - 10 * UNIT, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, wire_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(liq_before, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, bond_amount_of(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_requires_the_underwriter_authority, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before = liq_balance("alice"_n);

   BOOST_REQUIRE_EQUAL(error("missing authority of alice"),
                       push_bond("bob"_n, "accept"_n, mvo()
                          ("underwriter", "alice"_n)("request_id", 1)("amount", 4 * UNIT)));
   BOOST_REQUIRE_EQUAL(before, liq_balance("alice"_n));
   BOOST_REQUIRE(bond_rows_of(1).empty());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), accept("alice"_n, 99, 4 * UNIT));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(accept_refuses_a_held_request, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   const auto before = liq_balance("alice"_n);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(not_accepting_msg)), accept("alice"_n, 1, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(before, liq_balance("alice"_n));
   BOOST_REQUIRE(bond_rows_of(1).empty());
   BOOST_REQUIRE_EQUAL(STATE_HELD, request_state_of(1));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// approve
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(approve_refuses_before_the_window_and_when_unbonded, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(not_bonded_msg)), approve(1));

   // Partly bonded is still OPEN.
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(not_bonded_msg)), approve(1));

   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 6 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(window_open_msg)), approve(1));
   produce_block(fc::seconds(window_sec / 2));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(window_open_msg)), approve(1));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));
   BOOST_REQUIRE(request_row_of(1)["resolved_at"].as<time_point>() == time_point{});

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), approve(99));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(approve_after_the_window, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   const auto bonded_at = request_row_of(1)["bonded_at"].as<time_point>();
   pass_window(window_sec);

   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE(!notified("issuer"_n, "approve"_n));   // a state change only
   BOOST_REQUIRE(settlements().empty());
   const auto row = request_row_of(1);
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));
   const auto resolved_at = row["resolved_at"].as<time_point>();
   BOOST_REQUIRE(resolved_at >= bonded_at + fc::seconds(window_sec));

   // Approval is terminal: a second approve is refused and leaves the row as it was.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(not_bonded_msg)), approve(1));
   BOOST_REQUIRE(resolved_at == request_row_of(1)["resolved_at"].as<time_point>());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(approve_refuses_a_held_request, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   pass_window(window_sec);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(not_bonded_msg)), approve(1));
   BOOST_REQUIRE_EQUAL(STATE_HELD, request_state_of(1));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(addbounty_refuses_a_terminal_request, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), addbounty("issuer"_n, 1, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(0u, request_row_of(1)["bounty"].as_uint64());
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// hold
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(hold_in_open_charges_the_issuer_the_configured_share, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before      = liq_balance("issuer"_n);
   const auto bond_before = liq_balance(BOND_ACCOUNT);

   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE(!notified("issuer"_n, "hold"_n));
   BOOST_REQUIRE_EQUAL(before - 1 * UNIT, liq_balance("issuer"_n));   // 10% of 10 UNIT
   BOOST_REQUIRE_EQUAL(bond_before + 1 * UNIT, liq_balance(BOND_ACCOUNT));

   const auto row = request_row_of(1);
   BOOST_REQUIRE_EQUAL(STATE_HELD, request_state_of(1));
   BOOST_REQUIRE_EQUAL(1 * UNIT, row["hold_bond"].as_uint64());
   BOOST_REQUIRE_EQUAL("carol", row["hold_beneficiary"].as_string());
   BOOST_REQUIRE(row["held_at"].as<time_point>() != time_point{});
   BOOST_REQUIRE(row["resolved_at"].as<time_point>() == time_point{});

   const auto escrow = escrow_row_of(1, KIND_HOLD_BOND_BYTE);
   BOOST_REQUIRE(!escrow.is_null());
   BOOST_REQUIRE_EQUAL(KIND_HOLD_BOND, escrow["kind"].as_string());
   BOOST_REQUIRE_EQUAL("issuer", escrow["funder"].as_string());
   BOOST_REQUIRE_EQUAL(1 * UNIT, escrow["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(false, escrow["paid"].as_bool());
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(hold_in_bonded_and_after_setconfig, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(SYSIO_ACCOUNT, 2500));
   bonded_request(1, 'a');
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE_EQUAL(before - 2 * UNIT - UNIT / 2, liq_balance("issuer"_n));   // 25% of 10 UNIT
   BOOST_REQUIRE_EQUAL(STATE_HELD, request_state_of(1));
   BOOST_REQUIRE_EQUAL(2 * UNIT + UNIT / 2, request_row_of(1)["hold_bond"].as_uint64());
   BOOST_REQUIRE_EQUAL(2 * UNIT + UNIT / 2, escrow_row_of(1, KIND_HOLD_BOND_BYTE)["amount"].as_uint64());
   // The bond stays where it was.
   BOOST_REQUIRE_EQUAL(10 * UNIT, bond_amount_of(1, "alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(hold_in_wire_pulls_wire, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 0));
   const auto before     = wire_balance("issuer"_n);
   const auto liq_before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE_EQUAL(before - 1 * UNIT, wire_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(1 * UNIT, wire_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(liq_before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(1 * UNIT, escrow_row_of(1, KIND_HOLD_BOND_BYTE)["amount"].as_uint64());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(hold_refuses_after_approval_and_twice, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(hold_state_msg)), hold("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE_EQUAL(before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));
   BOOST_REQUIRE_EQUAL(0u, request_row_of(1)["hold_bond"].as_uint64());
   BOOST_REQUIRE(escrow_row_of(1, KIND_HOLD_BOND_BYTE).is_null());

   // A second hold on a HELD request is refused and charges nothing.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 2, "carol"_n));
   const auto held = liq_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(hold_state_msg)), hold("issuer"_n, 2, "bob"_n));
   BOOST_REQUIRE_EQUAL(held, liq_balance("issuer"_n));
   const auto row = request_row_of(2);
   BOOST_REQUIRE_EQUAL(1 * UNIT, row["hold_bond"].as_uint64());
   BOOST_REQUIRE_EQUAL("carol", row["hold_beneficiary"].as_string());
   BOOST_REQUIRE_EQUAL(1 * UNIT, escrow_row_of(2, KIND_HOLD_BOND_BYTE)["amount"].as_uint64());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(hold_requires_the_issuer, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(error("missing authority of issuer"), hold("alice"_n, 1, "alice"_n));
   BOOST_REQUIRE_EQUAL(before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE(escrow_row_of(1, KIND_HOLD_BOND_BYTE).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), hold("issuer"_n, 99, "carol"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(hold_refuses_a_beneficiary_that_is_not_an_account, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(beneficiary_msg)), hold("issuer"_n, 1, "nobody"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(beneficiary_msg)), hold("issuer"_n, 1, name{}));
   // The contract is an account, but a forfeit awarded to it could never be claimed out.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(beneficiary_self_msg)), hold("issuer"_n, 1, BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE(escrow_row_of(1, KIND_HOLD_BOND_BYTE).is_null());
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Rulings and claims: the payout table
// ---------------------------------------------------------------------------

// Shares throughout: covered 10 UNIT, alice 4, bob 6, bounty 1 UNIT, hold_bps 1000 -> hold bond 1 UNIT.

BOOST_FIXTURE_TEST_CASE(approved_returns_bonds_and_splits_the_bounty_by_stake, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 6 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const auto alice = liq_balance("alice"_n);
   const auto bob   = liq_balance("bob"_n);

   // Permissionless: a bystander claims for alice, and alice is paid.
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n, "carol"_n));
   BOOST_REQUIRE_EQUAL(alice + 4 * UNIT + 4 * UNIT / 10, liq_balance("alice"_n));
   BOOST_REQUIRE(raw_bond_of(1, "alice"_n).paid);
   BOOST_REQUIRE_EQUAL(6 * UNIT / 10, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);

   BOOST_REQUIRE_EQUAL(success(), claim(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(bob + 6 * UNIT + 6 * UNIT / 10, liq_balance("bob"_n));
   const auto paid = settlements();
   BOOST_REQUIRE_EQUAL(1u, paid.size());   // bond and bounty share in one transfer
   BOOST_REQUIRE_EQUAL("bob", paid.front().to.to_string());
   BOOST_REQUIRE_EQUAL(6 * UNIT + 6 * UNIT / 10, paid.front().quantity.get_amount());
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);
   BOOST_REQUIRE_EQUAL(0u, liq_balance(BOND_ACCOUNT));

   // Paid once only; an account with neither bond nor escrow, and a funder whose escrow earned no
   // yield, are owed nothing.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "carol"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "issuer"_n));
   BOOST_REQUIRE_EQUAL(alice + 4 * UNIT + 4 * UNIT / 10, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), claim(99, "alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(valid_after_a_hold_pays_the_hold_bond_to_the_underwriters, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 6 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   const auto issuer = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE(!notified("issuer"_n, "rslvvalid"_n));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE(request_row_of(1)["resolved_at"].as<time_point>() != time_point{});
   BOOST_REQUIRE(settlements().empty());   // a ruling sends nothing
   BOOST_REQUIRE_EQUAL(issuer, liq_balance("issuer"_n));
   // Fully bonded: no escrow awards anything outside the underwriters.
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).payout);
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).payout);

   const auto alice = liq_balance("alice"_n);
   const auto bob   = liq_balance("bob"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice + 4 * UNIT + 4 * UNIT / 10 + 4 * UNIT / 10, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(bob + 6 * UNIT + 6 * UNIT / 10 + 6 * UNIT / 10, liq_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).amount);

   // The challenger who held a valid statement is owed nothing.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "carol"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(valid_while_partly_bonded_pays_the_unbonded_share_to_sysio_and_issuer, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   const auto held_from = liq_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE_EQUAL(held_from - 1 * UNIT, liq_balance("issuer"_n));
   const auto issuer = liq_balance("issuer"_n);
   const auto sysio  = liq_balance(SYSIO_ACCOUNT);

   // The ruling sends nothing: it awards the unbonded 6/10 of each pot, the bounty's to sysio and the
   // hold bond's to the issuer.
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE(!notified("issuer"_n, "rslvvalid"_n));
   BOOST_REQUIRE(settlements().empty());
   BOOST_REQUIRE_EQUAL(sysio, liq_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(issuer, liq_balance("issuer"_n));
   auto bounty = raw_escrow_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, bounty.payee);
   BOOST_REQUIRE_EQUAL(6 * UNIT / 10, bounty.payout);
   BOOST_REQUIRE_EQUAL(1 * UNIT, bounty.amount);
   auto hold_bond = raw_escrow_of(1, KIND_HOLD_BOND_BYTE);
   BOOST_REQUIRE_EQUAL("issuer"_n, hold_bond.payee);
   BOOST_REQUIRE_EQUAL(6 * UNIT / 10, hold_bond.payout);

   // Each payee pulls its award.
   BOOST_REQUIRE_EQUAL(success(), claim(1, SYSIO_ACCOUNT, "carol"_n));
   BOOST_REQUIRE_EQUAL(sysio + 6 * UNIT / 10, liq_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n));
   BOOST_REQUIRE_EQUAL(issuer + 6 * UNIT / 10, liq_balance("issuer"_n));
   const auto paid = settlements();
   BOOST_REQUIRE_EQUAL(1u, paid.size());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, SYSIO_ACCOUNT, "carol"_n));
   // The escrows keep what remains for the underwriters.
   BOOST_REQUIRE_EQUAL(4 * UNIT / 10, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).payout);
   BOOST_REQUIRE_EQUAL(4 * UNIT / 10, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).amount);

   const auto alice = liq_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice + 4 * UNIT + 4 * UNIT / 10 + 4 * UNIT / 10, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).amount);
   BOOST_REQUIRE_EQUAL(0u, liq_balance(BOND_ACCOUNT));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(valid_with_nothing_bonded, sysio_bond_tester) try {
   // No hold: the whole bounty is sysio's to claim, and nothing else is owed.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   const auto issuer = liq_balance("issuer"_n);
   const auto sysio  = liq_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE(settlements().empty());
   BOOST_REQUIRE_EQUAL(success(), claim(1, SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(sysio + 1 * UNIT, liq_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(issuer, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(1u, settlements().size());
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "issuer"_n));

   // After a hold: the whole hold bond is the issuer's to claim back.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 2, "carol"_n));
   const auto held = liq_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(held, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(success(), claim(2, "issuer"_n));
   BOOST_REQUIRE_EQUAL(held + 1 * UNIT, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(0u, liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(2, "carol"_n));

   // In WIRE the bounty is claimed through sysio.token.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'c'), WIRE, 10 * UNIT, 1 * UNIT));
   const auto sysio_wire = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 3));
   BOOST_REQUIRE_EQUAL(sysio_wire, wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), claim(3, SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(sysio_wire + 1 * UNIT, wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0u, wire_balance(BOND_ACCOUNT));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(invalid_after_a_hold_forfeits_bonds_to_the_issuer_and_pays_the_challenger,
                        sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 6 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   const auto issuer = liq_balance("issuer"_n);
   const auto carol  = liq_balance("carol"_n);

   // The ruling sends nothing: it records the forfeit and awards both escrows to the challenger.
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE(!notified("issuer"_n, "rslvinvalid"_n));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE(settlements().empty());
   BOOST_REQUIRE_EQUAL(issuer, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, request_row_of(1)["forfeit_pending"].as_uint64());
   BOOST_REQUIRE_EQUAL("carol"_n, raw_escrow_of(1, KIND_BOUNTY_BYTE).payee);
   BOOST_REQUIRE_EQUAL(1 * UNIT, raw_escrow_of(1, KIND_BOUNTY_BYTE).payout);
   BOOST_REQUIRE_EQUAL("carol"_n, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).payee);
   BOOST_REQUIRE_EQUAL(1 * UNIT, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).payout);

   // Every bond reaches the issuer's claim as ONE transfer under the forfeit memo.
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n, "carol"_n));
   BOOST_REQUIRE_EQUAL(issuer + 10 * UNIT, liq_balance("issuer"_n));
   auto sent = settlements();
   BOOST_REQUIRE_EQUAL(1u, sent.size());
   BOOST_REQUIRE_EQUAL("issuer"_n, sent.front().to);
   BOOST_REQUIRE_EQUAL(10 * UNIT, sent.front().quantity.get_amount());
   BOOST_REQUIRE_EQUAL(0u, request_row_of(1)["forfeit_pending"].as_uint64());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "issuer"_n));

   // The challenger pulls the hold bond and the bounty in one transfer.
   BOOST_REQUIRE_EQUAL(success(), claim(1, "carol"_n));
   BOOST_REQUIRE_EQUAL(carol + 1 * UNIT + 1 * UNIT, liq_balance("carol"_n));
   sent = settlements();
   BOOST_REQUIRE_EQUAL(1u, sent.size());
   BOOST_REQUIRE_EQUAL(0u, liq_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_BOUNTY_BYTE).amount);
   BOOST_REQUIRE_EQUAL(0u, raw_escrow_of(1, KIND_HOLD_BOND_BYTE).amount);

   // Forfeited bonds that earned no yield leave the underwriters nothing to claim.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "carol"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(invalid_on_a_partly_bonded_request_forfeits_the_partial_bonds, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   const auto issuer = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE(!notified("issuer"_n, "rslvinvalid"_n));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE(settlements().empty());
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n));
   BOOST_REQUIRE_EQUAL(issuer + 4 * UNIT + 1 * UNIT, liq_balance("issuer"_n));   // forfeit and bounty returned

   // The forfeit and the returned bounty arrive as two transfers; only the forfeit carries its memo.
   const auto sent = settlements();
   BOOST_REQUIRE_EQUAL(2u, sent.size());
   BOOST_REQUIRE_EQUAL(4 * UNIT, sent.front().quantity.get_amount());
   BOOST_REQUIRE_EQUAL(1 * UNIT, sent.back().quantity.get_amount());
   BOOST_REQUIRE_EQUAL(0u, liq_balance(BOND_ACCOUNT));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(rulings_require_sysio_and_refuse_terminal_requests, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), rslvvalid("alice"_n, 1));
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), rslvinvalid("alice"_n, 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), rslvvalid(SYSIO_ACCOUNT, 99));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), rslvinvalid(SYSIO_ACCOUNT, 99));
   // Not terminal yet: nothing to claim.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));

   // Approval is terminal: no ruling follows it.
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvinvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));

   // Nor does a ruling follow a ruling.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvinvalid(SYSIO_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvvalid(SYSIO_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'c'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 3));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvvalid(SYSIO_ACCOUNT, 3));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(3));
} FC_LOG_AND_RETHROW()

// A HOLD_BOND escrow exists only when the hold bond is positive; a hold whose bond rounds to zero
// leaves none, and both rulings read that as "no hold bond".
BOOST_FIXTURE_TEST_CASE(a_missing_hold_bond_row_is_no_hold_bond, sysio_bond_tester) try {
   constexpr std::string_view LIQX = "LIQX";
   BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, LIQX, 2));
   BOOST_REQUIRE_EQUAL(success(), create_shadow(symbol::from_string("2,LIQX"), LIQX));

   // Precision 2: the increment is one base unit, and 1 * 1000 / 10000 rounds to zero.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQX, 1, 0));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE_EQUAL(0u, request_row_of(1)["hold_bond"].as_uint64());
   BOOST_REQUIRE(escrow_row_of(1, KIND_HOLD_BOND_BYTE).is_null());
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE(settlements().empty());

   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQX, 1, 0));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 2, "carol"_n));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(2));
   BOOST_REQUIRE(settlements().empty());

   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(request_row_of(2).is_null());
} FC_LOG_AND_RETHROW()

// An issuer that rejects every notification cannot block approval, a ruling or an underwriter's
// claim: none of them sends it anything. Only its own claim fails, and its forfeit stays recorded.
BOOST_FIXTURE_TEST_CASE(a_hostile_issuer_blocks_only_its_own_claim, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   // 1: approved. 2: held, then VALID while partly bonded, which awards the issuer part of its hold
   // bond. 3: INVALID with no hold, which forfeits a bond to the issuer and awards its bounty back.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 2, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 2, "carol"_n));
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'c'), LIQSOL, 10 * UNIT, 1 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 3, 6 * UNIT));

   set_code("issuer"_n, contracts::util::reject_all_wasm());
   produce_block();
   pass_window(window_sec);

   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(SYSIO_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(2));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 3));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(3));

   // The underwriters are paid.
   const auto alice = liq_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice + 10 * UNIT + 1 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim(2, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice + 11 * UNIT + 4 * UNIT + 4 * UNIT / 10 + 4 * UNIT / 10, liq_balance("alice"_n));
   const auto sysio = liq_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), claim(2, SYSIO_ACCOUNT, "carol"_n));
   BOOST_REQUIRE_EQUAL(sysio + 6 * UNIT / 10, liq_balance(SYSIO_ACCOUNT));

   // LIQ credits cannot invoke the issuer's rejecting code.
   const auto issuer = liq_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(3, "issuer"_n, "carol"_n));
   BOOST_REQUIRE(!notified("issuer"_n, "settle"_n));
   BOOST_REQUIRE_EQUAL(success(), claim(2, "issuer"_n, "carol"_n));
   BOOST_REQUIRE(!notified("issuer"_n, "settle"_n));
   BOOST_REQUIRE_EQUAL(issuer + 7 * UNIT + 6 * UNIT / 10, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(0u, request_row_of(3)["forfeit_pending"].as_uint64());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Yield
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(unbacked_yield_survives_pruning_and_never_repays_principal, sysio_bond_tester) try {
   bonded_request(1, 'a', 3600);
   const auto start = liq_index();
   uint64_t earned = 0, received = 0;
   for (unsigned i = 0; i < 20 && earned <= received; ++i) {
      BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, UNIT));
      BOOST_REQUIRE_EQUAL(success(), sweepyield(LIQSOL));
      earned = static_cast<uint64_t>(static_cast<fc::uint128_t>(10 * UNIT) * (liq_index() - start) / YIELD_INDEX_SCALE);
      received = pool_row_of()["pool"]["received"].as_uint64();
   }
   BOOST_REQUIRE_GT(earned, received);
   pass_window(3600);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const auto principal_before = liq_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(principal_before + 10 * UNIT, liq_balance("alice"_n));
   const auto residual = raw_bond_of(1, "alice"_n).yield.owed_wire;
   BOOST_REQUIRE_GT(residual, 0u);
   BOOST_REQUIRE(raw_bond_of(1, "alice"_n).paid);
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 100));
   BOOST_REQUIRE(!request_row_of(1).is_null());
   BOOST_REQUIRE_EQUAL(residual, raw_bond_of(1, "alice"_n).yield.owed_wire);
   // Unattributed custody earns slack that can back the already-recorded residual.
   BOOST_REQUIRE_EQUAL(success(), mint(BOND_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, UNIT));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(principal_before + 10 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, raw_bond_of(1, "alice"_n).yield.owed_wire);
   const auto wire_before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(wire_before + earned, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 100));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(rejected_wire_cannot_block_liq_or_expire_with_request, sysio_bond_tester) try {
   bonded_request(1, 'a', 3600);
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   pass_window(3600);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const auto before_liq = liq_balance("alice"_n);
   const auto before_wire = wire_balance("alice"_n);
   const auto earned = liq_owed(BOND_ACCOUNT);
   set_code("alice"_n, contracts::util::block_transfer_wasm());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(before_liq + 10 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(before_wire, wire_balance("alice"_n));
   BOOST_REQUIRE(!notified("alice"_n, "settle"_n));
   BOOST_REQUIRE(!notified("alice"_n, "transfer"_n));
   BOOST_REQUIRE(claimwire("alice"_n) != success());
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 100));
   set_code("alice"_n, std::vector<uint8_t>{});
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(before_wire + earned, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claimwire("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(bond_yield_is_paid_with_the_bond, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);   // alice bonds 10 UNIT of LIQSOL, no bounty
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));

   // What sysio.liq owes sysio.bond's row -- custody::custodian_owed's formula over the accounts row.
   const uint64_t owed = liq_owed(BOND_ACCOUNT);
   BOOST_REQUIRE(owed > 0);
   const auto bond_wire = wire_balance(BOND_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), sweepyield(LIQSOL));
   BOOST_REQUIRE_EQUAL(bond_wire + owed, wire_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(owed, pool_row_of()["pool"]["received"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, liq_owed(BOND_ACCOUNT));

   const auto alice_liq  = liq_balance("alice"_n);
   const auto alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice_liq + 10 * UNIT, liq_balance("alice"_n));
   // alice is the only bond: she is paid exactly what was pulled.
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + owed, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(bond_wire, wire_balance(BOND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(owed, pool_row_of()["pool"]["credited"].as_uint64());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
} FC_LOG_AND_RETHROW()

// With no sweep, `claim` pulls the pool itself, and a returned bond, still held until it is claimed,
// keeps earning up to the claim: alice, the only holder, receives what sysio.bond's row is owed then.
BOOST_FIXTURE_TEST_CASE(claim_pulls_the_yield_itself_up_to_the_claim, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const uint64_t at_ruling = liq_owed(BOND_ACCOUNT);
   BOOST_REQUIRE(at_ruling > 0);

   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   const uint64_t at_claim = liq_owed(BOND_ACCOUNT);
   BOOST_REQUIRE(at_claim > at_ruling);

   const auto alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + at_claim, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, liq_owed(BOND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// An escrow keeps earning after an APPROVED ruling while it holds shares its underwriters have not
// claimed; its funder is paid that yield, and may claim again until the escrow holds nothing.
BOOST_FIXTURE_TEST_CASE(escrow_yield_after_approval_reaches_the_funder, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   const auto start = liq_index();
   // Nothing distributed yet: the funder is owed nothing.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "issuer"_n));

   // A distribution after the ruling reaches the escrow's 1 UNIT.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   const auto mid = liq_index();
   const auto first = static_cast<uint64_t>(static_cast<fc::uint128_t>(1 * UNIT) * (mid - start) / YIELD_INDEX_SCALE);
   BOOST_REQUIRE(first > 0);
   auto issuer_wire = wire_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n));
   BOOST_REQUIRE_EQUAL(success(), claimwire("issuer"_n));
   BOOST_REQUIRE_EQUAL(issuer_wire + first, wire_balance("issuer"_n));
   auto escrow = raw_escrow_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE_EQUAL(1 * UNIT, escrow.amount);   // alice's share is still held
   BOOST_REQUIRE(!escrow.paid);

   // It keeps earning until alice claims her share out of it.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   const auto late   = liq_index();
   const auto second = static_cast<uint64_t>(static_cast<fc::uint128_t>(1 * UNIT) * (late - mid) / YIELD_INDEX_SCALE);
   BOOST_REQUIRE(second > 0);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   escrow = raw_escrow_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE_EQUAL(0u, escrow.amount);
   BOOST_REQUIRE_EQUAL(second, escrow.yield.owed_wire);

   issuer_wire = wire_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n));
   // The pool is first-come-first-served; the last taker may be short one atomic unit of rounding.
   BOOST_REQUIRE_EQUAL(success(), claimwire("issuer"_n));
   const auto second_paid = wire_balance("issuer"_n) - issuer_wire;
   BOOST_REQUIRE(second_paid <= second && second_paid + 1 >= second);
   BOOST_REQUIRE(raw_escrow_of(1, KIND_BOUNTY_BYTE).paid);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "issuer"_n));

   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(forfeited_bond_yield_returns_to_the_ledger_as_bonus_yield, sysio_bond_tester) try {
   bonded_request(1, 'a');   // alice bonds 10 UNIT of LIQSOL
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(success(), sweepyield(LIQSOL));
   const uint64_t pulled = pool_row_of()["pool"]["received"].as_uint64();
   BOOST_REQUIRE(pulled > 0);

   const auto alice_wire = wire_balance("alice"_n);
   const auto alice_liq  = liq_balance("alice"_n);
   const auto bob_owed   = liq_owed("bob"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   // The underwriter receives nothing; the forfeited bond's WIRE goes to sysio.liq for LIQSOL.
   BOOST_REQUIRE_EQUAL(alice_wire, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_liq, liq_balance("alice"_n));
   BOOST_REQUIRE(settlements().empty());
   const auto added = addyields();
   BOOST_REQUIRE_EQUAL(1u, added.size());
   BOOST_REQUIRE_EQUAL("sysio.bond", added.front().first.to_string());
   BOOST_REQUIRE_EQUAL(pulled, added.front().second.get_amount());
   BOOST_REQUIRE(raw_bond_of(1, "alice"_n).paid);

   // bob, a LIQSOL holder outside the contract, earns from it.
   const auto bob_after = liq_owed("bob"_n);
   BOOST_REQUIRE(bob_after > bob_owed);
   const auto bob_wire = wire_balance("bob"_n);
   BOOST_REQUIRE_EQUAL(success(), liq_claim("bob"_n));
   BOOST_REQUIRE_EQUAL(bob_wire + bob_after, wire_balance("bob"_n));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
} FC_LOG_AND_RETHROW()

// Once every bond is paid, the funder's claim marks its escrow paid even though the division
// remainder is still in it: that remainder is slack, and the request can be pruned.
BOOST_FIXTURE_TEST_CASE(escrow_is_paid_once_every_bond_is_paid_despite_a_remainder, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   // A bounty of 1 UNIT and one base unit splits 3:7 with one base unit left over.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT + 1,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 7 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));

   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "bob"_n));
   auto escrow = raw_escrow_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE_EQUAL(1u, escrow.amount);
   BOOST_REQUIRE(!escrow.paid);

   const auto issuer_wire = wire_balance("issuer"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n));
   BOOST_REQUIRE_EQUAL(success(), claimwire("issuer"_n));
   BOOST_REQUIRE_GT(wire_balance("issuer"_n), issuer_wire);
   escrow = raw_escrow_of(1, KIND_BOUNTY_BYTE);
   BOOST_REQUIRE_EQUAL(1u, escrow.amount);
   BOOST_REQUIRE(escrow.paid);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "issuer"_n));

   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());
} FC_LOG_AND_RETHROW()

// sysio.liq refuses a distribution to a symbol with no supply. A forfeited bond's WIRE claimed then
// stays in the contract, and the bond is still marked paid.
BOOST_FIXTURE_TEST_CASE(forfeited_bond_yield_stays_when_the_symbol_has_no_supply, sysio_bond_tester) try {
   constexpr std::string_view LIQX     = "LIQX";
   const symbol               LIQX_SYM = symbol::from_string("9,LIQX");
   BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, LIQX, 9));
   BOOST_REQUIRE_EQUAL(success(), create_shadow(LIQX_SYM, LIQX));
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 10 * UNIT, LIQX));

   // The only LIQX there is backs alice's bond, earns yield, and is forfeited.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQX, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT, LIQX_SYM));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 1));

   // The issuer takes the forfeit and burns it: LIQX has no supply left.
   BOOST_REQUIRE_EQUAL(success(), claim(1, "issuer"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, liq_balance("issuer"_n, LIQX_SYM));
   BOOST_REQUIRE_EQUAL(success(), burn_shadow("issuer"_n, 10 * UNIT, LIQX_SYM, LIQX));
   BOOST_REQUIRE_EQUAL(0u, liq_balance("issuer"_n, LIQX_SYM));

   // The issuer's claim already pulled the WIRE the forfeited bond earned into the pool.
   const auto bond_wire = wire_balance(BOND_ACCOUNT);
   const auto earned    = pool_row_of(LIQX)["pool"]["received"].as_uint64();
   BOOST_REQUIRE_GT(earned, 0u);
   BOOST_REQUIRE_EQUAL(earned, bond_wire);
   BOOST_REQUIRE_EQUAL(0u, pool_row_of(LIQX)["pool"]["credited"].as_uint64());

   const auto alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE(addyields().empty());
   BOOST_REQUIRE_EQUAL(alice_wire, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(bond_wire, wire_balance(BOND_ACCOUNT));   // kept in the contract
   BOOST_REQUIRE_EQUAL(earned, pool_row_of(LIQX)["pool"]["credited"].as_uint64());
   BOOST_REQUIRE(raw_bond_of(1, "alice"_n).paid);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(sweepyield_refuses_wire, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(wire_no_yield_msg)), sweepyield(WIRE));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(unsupported_token_msg)), sweepyield("NOPE"));
   // Nothing owed yet: a sweep is a no-op.
   BOOST_REQUIRE_EQUAL(success(), sweepyield(LIQSOL));

   // A WIRE request's claim pays the bond and its share, and no yield on top, even while LIQSOL earns.
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), WIRE, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL("0", request_row_of(1)["resolved_index"].as_string());
   const auto alice = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice + 11 * UNIT, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(1u, transfers_on(TOKEN_ACCOUNT, token_abi_ser).size());
   BOOST_REQUIRE_EQUAL(0u, wire_balance(BOND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// prune
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(prune_erases_only_paid_out_terminal_requests, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   // 1: approved and fully claimed.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   // 2: approved, its bond not claimed yet.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 2, 10 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL(success(), approve(2));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   // 3: forfeited with no yield earned: once the issuer has claimed the forfeit, nothing is owed.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'c'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 3, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 3));
   // 4: still OPEN.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'd'), LIQSOL, 10 * UNIT, 0));

   // The forfeit not yet claimed keeps request 3.
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(3));
   BOOST_REQUIRE_EQUAL(success(), claim(3, "issuer"_n));

   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(bond_rows_of(1).empty());
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());
   BOOST_REQUIRE(request_row_of(3).is_null());
   BOOST_REQUIRE(bond_rows_of(3).empty());
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(2));
   BOOST_REQUIRE_EQUAL(1u, bond_rows_of(2).size());
   BOOST_REQUIRE(!escrow_row_of(2, KIND_BOUNTY_BYTE).is_null());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(4));

   // Once bob claims, request 2 goes too; the OPEN one stays.
   BOOST_REQUIRE_EQUAL(success(), claim(2, "bob"_n));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(2).is_null());
   BOOST_REQUIRE(bond_rows_of(2).empty());
   BOOST_REQUIRE(escrow_row_of(2, KIND_BOUNTY_BYTE).is_null());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(4));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(prune_stops_at_its_limit, sysio_bond_tester) try {
   for (char fill : {'a', 'b', 'c'}) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, fill), LIQSOL, 10 * UNIT, 0));
   }
   for (uint64_t id = 1; id <= 3; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, id));
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 2));
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(request_row_of(2).is_null());
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(3));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 2));
   BOOST_REQUIRE(request_row_of(3).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(prune_starts_at_from_id, sysio_bond_tester) try {
   for (char fill : {'a', 'b', 'c'}) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, fill), LIQSOL, 10 * UNIT, 0));
   }
   for (uint64_t id = 1; id <= 3; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, id));
   pass_retention();

   // From id 2: request 1, prunable, is left; 2 and 3 go.
   BOOST_REQUIRE_EQUAL(success(), prune(2, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE(request_row_of(2).is_null());
   BOOST_REQUIRE(request_row_of(3).is_null());
   // Past every request: nothing is erased.
   BOOST_REQUIRE_EQUAL(success(), prune(4, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), prune(1, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

// A request whose token no longer resolves cannot be settled: prune skips it instead of failing,
// and still erases the prunable requests after it.
BOOST_FIXTURE_TEST_CASE(prune_skips_a_request_whose_token_no_longer_resolves, sysio_bond_tester) try {
   for (char fill : {'a', 'b'}) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, fill), LIQSOL, 10 * UNIT, 0));
   }
   for (uint64_t id = 1; id <= 2; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, id));
   pass_retention();
   rewrite_request_token(1, "NOPE");
   produce_block();

   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE_EQUAL("NOPE", request_row_of(1)["token_code"].as_string());
   BOOST_REQUIRE(request_row_of(2).is_null());
} FC_LOG_AND_RETHROW()

// A ruled request stays PRUNE_RETENTION_SEC after its ruling even when nothing is owed on it, so an
// issuer that polls the row rather than being notified always sees the ruling.
BOOST_FIXTURE_TEST_CASE(prune_keeps_a_ruling_for_the_retention_period, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 1));

   // Nothing is owed on it at once -- nothing was bonded -- but the ruling is kept.
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   produce_block(fc::seconds(PRUNE_RETENTION_SEC - 10));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));

   // Once the retention period has passed, it goes.
   produce_block(fc::seconds(10));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

/// Durable results survive a fully paid settlement and arbitrary delay until the authenticated issuer
/// acknowledges; acknowledgement itself neither settles a request nor blocks independent claims.
BOOST_FIXTURE_TEST_CASE(durable_outcome_requires_issuer_acknowledgement, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), push_bond("issuer"_n, "requestkeep"_n, mvo()
      ("issuer", "issuer"_n)("schema", "oppenvelope"_n)("statement", bytes(32, 'a'))
      ("token_code", codename_mvo(LIQSOL))("covered", 10 * UNIT)("bounty", 0)("window_sec", WINDOW_SEC)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("request is not resolved"),
                       push_bond("issuer"_n, "ack"_n, mvo()("request_id", 1)));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(SYSIO_ACCOUNT, 1));
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE_NE(success(), push_bond("alice"_n, "ack"_n, mvo()("request_id", 1)));
   BOOST_REQUIRE_EQUAL(success(), push_bond("issuer"_n, "ack"_n, mvo()("request_id", 1)));
   BOOST_REQUIRE_EQUAL(success(), push_bond("issuer"_n, "ack"_n, mvo()("request_id", 1)));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

// A request still owed a claim keeps its row, but the bond rows already paid out go: one unclaimed
// payout holds no other underwriter's RAM, and the unpaid rows claim exactly as before.
BOOST_FIXTURE_TEST_CASE(prune_erases_the_paid_bonds_of_a_request_it_keeps, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 6 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));

   // Within the retention period: alice's paid row goes at once, bob's stays, and so does the request.
   const auto alice_ram = ram_usage("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(1u, bond_rows_of(1).size());
   BOOST_REQUIRE_EQUAL(6 * UNIT, bond_amount_of(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));
   BOOST_REQUIRE_LT(ram_usage("alice"_n), alice_ram);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));

   // Past it, bob's unclaimed bond still keeps the request.
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));

   // Bob is paid his bond and his stake's share of the bounty as if nothing had been erased.
   const auto bob_before = liq_balance("bob"_n);
   BOOST_REQUIRE_EQUAL(success(), claim(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(bob_before + 6 * UNIT + 6 * UNIT / 10, liq_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(bond_rows_of(1).empty());
   BOOST_REQUIRE(escrow_row_of(1, KIND_BOUNTY_BYTE).is_null());
} FC_LOG_AND_RETHROW()

// Review focus 2, sysio.bond: while the sysio.andon cord is pulled a bond is paid in -- request, accept,
// hold and the rulings run -- and every payout waits: `claim` is refused, and what it owes stays recorded.
// After the clear the same claim pays in full.
BOOST_FIXTURE_TEST_CASE(a_freeze_takes_bonds_in_and_defers_every_claim, sysio_bond_tester) try {
   abi_serializer andon_abi_ser;
   sysio_system::test_support::andon::deploy(*this, andon_abi_ser, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), sysio_system::test_support::andon::pull(*this, andon_abi_ser));
   const auto frozen = wasm_assert_msg(sysio_system::test_support::andon::frozen_message);

   constexpr uint32_t window_sec = 3600;
   const auto         alice      = liq_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(alice - 10 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(11 * UNIT, liq_balance(BOND_ACCOUNT));   // the bond and the bounty, both in
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));

   BOOST_REQUIRE_EQUAL(frozen, claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(frozen, claim(1, "alice"_n, "carol"_n));
   BOOST_REQUIRE(!raw_bond_of(1, "alice"_n).paid);
   BOOST_REQUIRE_EQUAL(11 * UNIT, liq_balance(BOND_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), sysio_system::test_support::andon::clear(*this, andon_abi_ser));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(alice + UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE(raw_bond_of(1, "alice"_n).paid);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
