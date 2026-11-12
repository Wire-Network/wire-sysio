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

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
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
constexpr std::string_view first_bond_msg        = "first bond is below the request's minimum";
constexpr std::string_view not_accepting_msg     = "request is not accepting bonds";
constexpr std::string_view terminal_msg          = "request is already resolved";
constexpr std::string_view not_bonded_msg        = "request is not bonded";
constexpr std::string_view window_open_msg       = "challenge window has not passed";
constexpr std::string_view hold_state_msg        = "hold is only possible while the request is OPEN or BONDED";
constexpr std::string_view beneficiary_msg       = "hold beneficiary is not an account";
constexpr std::string_view beneficiary_self_msg  = "hold beneficiary cannot be the contract";
constexpr std::string_view nothing_msg           = "nothing to claim";
constexpr std::string_view wire_no_yield_msg     = "WIRE collateral earns no shadow yield";
constexpr std::string_view prune_limit_msg       = "limit exceeds the most requests one prune examines";

// The chain's own refusals of a declared authority, as the authorization manager words them.
constexpr std::string_view unsatisfied_authority_msg = "but does not have signatures for it";
constexpr std::string_view irrelevant_authority_msg  = "declares irrelevant authority";
constexpr std::string_view missing_permission_msg    = "authorizations include a non-existent permission";


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
/// Mirrors the contract's MAX_BONDS_PER_REQUEST.
constexpr uint64_t MAX_BONDS_PER_REQUEST = 64;

/// Mirrors the contract's MAX_PRUNE_LIMIT.
constexpr uint32_t MAX_PRUNE_LIMIT = 64;

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
/// sysio.liq as the shadow custody contract and yield source, and sysio.synd as the signer that
/// mints shadow LIQ (an account only). The treasury supply of WIRE sits on `sysio`, as the bootstrap leaves it.
class sysio_bond_tester : public tester {
public:
   static constexpr auto BOND_ACCOUNT   = "sysio.bond"_n;
   static constexpr auto LIQ_ACCOUNT    = "sysio.liq"_n;
   static constexpr auto SYND_ACCOUNT   = "sysio.synd"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto SYSIO_ACCOUNT  = "sysio"_n;
   /// The account the rulings' native permissions delegate to beside `sysio`.
   static constexpr auto JUDGE_ACCOUNT  = "bond.judge"_n;
   /// The rulings; each is linked to the `sysio.bond` permission of the same name.
   static constexpr auto RSLVVALID      = "rslvvalid"_n;
   static constexpr auto RSLVINVALID    = "rslvinvalid"_n;

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
                        JUDGE_ACCOUNT, "issuer"_n, "alice"_n, "bob"_n, "carol"_n });
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

      // Shadow LIQSOL for the issuer and two underwriters, minted by sysio.synd, the only minter.
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
   /// Push one sysio.bond action authorized and signed by every one of `signers`, in its own block.
   action_result push_bond_signed_by(const std::vector<account_name>& signers, name action_name,
                                     const variant_object& data) {
      try {
         last_trace = push_action(BOND_ACCOUNT, action_name, signers, data);
         produce_block();
         return success();
      } catch (const fc::exception& ex) {
         last_trace.reset();
         return error(ex.top_message());
      }
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
   /// Mint `amount` of the shadow of `token_code` (LIQSOL by default) to `account`, signed as
   /// sysio.synd.
   action_result mint(name account, uint64_t amount, std::string_view token_code = LIQSOL) {
      return push(LIQ_ACCOUNT, liq_abi_ser, SYND_ACCOUNT, "mint"_n, mvo()
         ("to", account)("token_code", codename_mvo(token_code))("amount", amount));
   }
   /// Distribute `amount` WIRE from `from` to the holders of `sym` (LIQSOL by default) through
   /// sysio.liq, raising its index.
   action_result addyield(name from, uint64_t amount, symbol sym = LIQSOL_SYM) {
      return push(LIQ_ACCOUNT, liq_abi_ser, from, "addyield"_n, mvo()
         ("from", from)("quantity", asset(static_cast<int64_t>(amount), WIRE_SYM))
         ("target", sym.to_symbol_code()));
   }
   /// Burn `amount` of `holder`'s shadow `sym` (the shadow of registry token `token_code`) the way a
   /// de-syndication does: the holder moves it to sysio.synd, which burns it out of its own row.
   action_result burn_shadow(name holder, uint64_t amount, symbol sym, std::string_view token_code) {
      auto r = push(LIQ_ACCOUNT, liq_abi_ser, holder, "transfer"_n, mvo()
         ("from", holder)("to", SYND_ACCOUNT)("quantity", asset(static_cast<int64_t>(amount), sym))("memo", ""));
      if (r != success()) return r;
      return push(LIQ_ACCOUNT, liq_abi_ser, SYND_ACCOUNT, "burn"_n, mvo()
         ("token_code", codename_mvo(token_code))("amount", amount));
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
   /// `hold` signed by `signer` and, when it is an account, by `beneficiary`, whose RAM the hold-bond
   /// escrow bills.
   action_result hold(name signer, uint64_t request_id, name beneficiary) {
      std::vector<account_name> signers{signer};
      if (beneficiary != signer && control->db().find<account_object, by_name>(beneficiary) != nullptr)
         signers.push_back(beneficiary);
      return push_bond_signed_by(signers, "hold"_n, mvo()("request_id", request_id)("beneficiary", beneficiary));
   }
   /// `hold` signed by `signer` alone, without the beneficiary's authority.
   action_result hold_without_beneficiary(name signer, uint64_t request_id, name beneficiary) {
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
   /// Push ruling `action_name` declaring `sysio.bond@permission`, signed with `signer`'s key: how a
   /// delegate rules through a linked native permission.
   action_result rule_through(name action_name, name signer, name permission, uint64_t request_id) {
      return sysio_system::test_support::push_contract_action_and_produce_block(
         *this, BOND_ACCOUNT, bond_abi_ser, signer, action_name, mvo()("request_id", request_id), last_trace,
         {{BOND_ACCOUNT, permission}});
   }
   /// The authority any one of `delegates` satisfies alone through its `active`, in chain name order.
   static authority delegated_authority(std::vector<name> delegates) {
      std::sort(delegates.begin(), delegates.end());
      authority auth{1, {}, {}};
      for (auto delegate : delegates) auth.accounts.push_back({{delegate, config::active_name}, 1});
      return auth;
   }
   /// Replace the delegates of `sysio.bond@permission`, a child of `active`; the account's `active`
   /// signs the updateauth.
   void delegate_ruling(name permission, std::vector<name> delegates) {
      set_authority(BOND_ACCOUNT, permission, delegated_authority(std::move(delegates)), config::active_name);
      produce_block();
   }
   /// Link each ruling to the `sysio.bond` permission of the same name; the account's `owner` signs.
   void link_rulings() {
      signed_transaction trx;
      for (auto ruling : {RSLVVALID, RSLVINVALID})
         trx.actions.emplace_back(std::vector<permission_level>{{BOND_ACCOUNT, config::owner_name}},
                                  linkauth{BOND_ACCOUNT, BOND_ACCOUNT, ruling, ruling});
      set_transaction_headers(trx);
      trx.sign(get_private_key(BOND_ACCOUNT, "owner"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }
   /// Give each ruling its native permission, delegated to `JUDGE_ACCOUNT` and `sysio`, and link the
   /// ruling to it, as the bootstrap does.
   void delegate_rulings() {
      for (auto ruling : {RSLVVALID, RSLVINVALID}) delegate_ruling(ruling, {JUDGE_ACCOUNT, SYSIO_ACCOUNT});
      link_rulings();
   }
   /// Rewrite `sysio.bond@permission` to `delegates`, declared by that same permission and signed with
   /// `signer`'s key: the native rule that a permission may rewrite itself.
   action_result rewrite_ruling_permission(name signer, name permission, std::vector<name> delegates) {
      try {
         signed_transaction trx;
         trx.actions.emplace_back(std::vector<permission_level>{{BOND_ACCOUNT, permission}},
                                  updateauth{BOND_ACCOUNT, permission, config::active_name,
                                             delegated_authority(std::move(delegates))});
         set_transaction_headers(trx);
         trx.sign(get_private_key(signer, "active"), control->get_chain_id());
         push_transaction(trx);
         produce_block();
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }
   /// Require `result` to be a refusal whose message contains `fragment`.
   static void require_refusal(const action_result& result, std::string_view fragment) {
      BOOST_REQUIRE_MESSAGE(result != success() && result.find(fragment) != std::string::npos,
                            "expected a refusal containing '" << fragment << "', got: " << result);
   }
   /// An unbonded OPEN request for `statement_fill`'s statement: the next request id.
   void open_request(char statement_fill) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, statement_fill), LIQSOL,
                                             10 * UNIT, 0));
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
                : ser.binary_to_variant(type, data, fixture_abi_yield_function());
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
      const auto                     table_id = compute_table_id(table.to_uint64_t());
      std::vector<std::vector<char>> rows;
      auto cursor = control->kv().open_row_cursor();
      chain::kv_row row;
      for (bool more = cursor->seek_lower({BOND_ACCOUNT, table_id, prefix}, row);
           more && row.code == BOND_ACCOUNT && row.table_id == table_id && row.key.starts_with(prefix);
           more = cursor->next(row))
         rows.emplace_back(row.value.begin(), row.value.end());
      return rows;
   }
   /// The kv row of sysio.bond's `table` whose key is exactly `key`; null when there is none.
   std::optional<chain::kv_row> find_kv_row(name table, const std::string& key) {
      return control->kv().find_row(BOND_ACCOUNT, compute_table_id(table.to_uint64_t()), key);
   }
   /// The kv key of request `id`: the id as a big-endian word.
   static std::string request_key_of(uint64_t id) {
      std::string key;
      append_key_word(key, id);
      return key;
   }
   /// The ids of at most `count` requests from id `from` up, in id order: what a crank reads to place its
   /// next prune.
   std::vector<uint64_t> request_ids_from(uint64_t from, size_t count) {
      const auto            table_id = compute_table_id("requests"_n.to_uint64_t());
      std::vector<uint64_t> ids;
      auto cursor = control->kv().open_row_cursor();
      chain::kv_row row;
      for (bool more = cursor->seek_lower({BOND_ACCOUNT, table_id, request_key_of(from)}, row);
           more && ids.size() < count && row.code == BOND_ACCOUNT && row.table_id == table_id;
           more = cursor->next(row))
         ids.push_back(decode(bond_abi_ser, "request_row", std::vector<char>(row.value.begin(), row.value.end()))["id"]
                          .as_uint64());
      return ids;
   }
   /// The account that pays the RAM of the kv row of `table` keyed `key`; the test fails without one.
   name payer_of(name table, const std::string& key) {
      const auto row = find_kv_row(table, key);
      BOOST_REQUIRE_MESSAGE(row.has_value(), "no " << table << " row");
      return row->payer;
   }
   /// The RAM `account` uses, in bytes.
   int64_t ram_usage(name account) { return control->get_resource_limits_manager().get_account_ram_usage(account); }
   /// Cap `account`'s RAM at what it uses now, so any further allocation it pays for fails.
   void exhaust_ram(name account) {
      control->get_mutable_resource_limits_manager().set_account_limits(account, ram_usage(account), -1, -1, false);
   }

   // --- the RAM ledger ---

   /// One payer's bill for one KV table: the payer, the contract and the table (or secondary index) id.
   using bill_key = std::tuple<name, name, uint16_t>;
   /// Bytes billed per payer, contract and table.
   using ram_bill = std::map<bill_key, int64_t>;

   /// The bill of payer `payer` for `code`'s table `table`.
   static bill_key bill_of(name payer, name code, name table) {
      return {payer, code, compute_table_id(table.to_uint64_t())};
   }
   /// The bill of payer `payer` for the secondary index `index` of `code`'s table `table`.
   static bill_key bill_of(name payer, name code, name table, name index) {
      return {payer, code, compute_sec_table_id(table.to_uint64_t(), index.to_uint64_t())};
   }
   /// Every KV row and secondary index entry of the chain, billed as the chain bills them: the key and
   /// the value of a row, or both keys of an index entry, plus the fixed per-row overhead.
   ram_bill ram_bill_now() const {
      ram_bill bill;
      auto rows = control->kv().open_row_cursor();
      chain::kv_row row;
      for (bool more = rows->seek_lower({name{}, 0, {}, {}}, row); more; more = rows->next(row)) {
         bill[{row.payer, row.code, row.table_id}] +=
            static_cast<int64_t>(row.key.size() + row.value.size() + config::kv_row_overhead_ram_bytes);
      }
      auto entries = control->kv().open_index_cursor();
      chain::kv_index_entry entry;
      for (bool more = entries->seek_lower({name{}, 0, {}, {}}, entry); more; more = entries->next(entry)) {
         bill[{entry.payer, entry.code, entry.table_id}] += static_cast<int64_t>(
            entry.sec_key.size() + entry.pri_key.size() + config::kv_index_entry_overhead_ram_bytes);
      }
      return bill;
   }
   /// A readable list of a bill, for failure messages.
   static std::string describe(const ram_bill& bill) {
      std::ostringstream out;
      for (const auto& [key, bytes] : bill)
         out << " [" << std::get<0>(key) << " " << std::get<1>(key) << " table " << std::get<2>(key) << ": "
             << bytes << "]";
      return out.str();
   }
   /// The accounts whose RAM every ledger check reconciles.
   std::vector<name> ledger_accounts() const {
      return {SYSIO_ACCOUNT, BOND_ACCOUNT, TOKENS_ACCOUNT, "issuer"_n, "alice"_n, "bob"_n, "carol"_n};
   }
   /// Run `step` and return what it billed, per payer and table. Checks first that the ledger closes:
   /// each of `ledger_accounts()` moved its RAM usage by exactly what the KV delta bills it, so the delta
   /// is everything the step billed them.
   ram_bill billed_by(const std::function<void()>& step) {
      std::map<name, int64_t> usage;
      for (const name account : ledger_accounts()) usage[account] = ram_usage(account);
      const ram_bill before = ram_bill_now();
      step();
      const ram_bill after = ram_bill_now();
      ram_bill delta;
      for (const auto& [key, bytes] : after) {
         const auto it = before.find(key);
         const int64_t change = bytes - (it == before.end() ? 0 : it->second);
         if (change != 0) delta[key] = change;
      }
      for (const auto& [key, bytes] : before) {
         if (!after.contains(key)) delta[key] = -bytes;
      }
      for (const name account : ledger_accounts()) {
         int64_t billed = 0;
         for (const auto& [key, bytes] : delta) {
            if (std::get<0>(key) == account) billed += bytes;
         }
         BOOST_REQUIRE_MESSAGE(ram_usage(account) - usage[account] == billed,
                               account << " RAM moved by " << ram_usage(account) - usage[account]
                                       << " but its KV rows by " << billed << ";" << describe(delta));
      }
      return delta;
   }
   /// True iff `key` is a row the `sysio` RAM pool pays for as shared infrastructure: sysio.bond's
   /// configuration, counter and yield pools, and the custody rows sysio.liq and sysio.token keep.
   bool is_shared(const bill_key& key) const {
      const auto [payer, code, table] = key;
      if (payer != SYSIO_ACCOUNT) return false;
      if (code == LIQ_ACCOUNT || code == TOKEN_ACCOUNT) return true;
      return code == BOND_ACCOUNT && (table == compute_table_id("bondconfig"_n.to_uint64_t()) ||
                                      table == compute_table_id("bondcounters"_n.to_uint64_t()) ||
                                      table == compute_table_id("yieldpool"_n.to_uint64_t()));
   }
   /// Require `delta` to bill exactly `activity` -- each table growing (+1) or refunded (-1) -- and
   /// nothing else but shared rows of the `sysio` RAM pool. Returns those shared rows.
   ram_bill require_billed(const ram_bill& delta, const std::map<bill_key, int>& activity) {
      ram_bill shared;
      for (const auto& [key, bytes] : delta) {
         const auto it = activity.find(key);
         if (it != activity.end()) {
            BOOST_REQUIRE_MESSAGE((bytes > 0 ? 1 : -1) == it->second, "wrong direction:" << describe(delta));
         } else {
            BOOST_REQUIRE_MESSAGE(is_shared(key), "unexpected bill [" << std::get<0>(key) << " " << std::get<1>(key)
                                                     << " table " << std::get<2>(key) << "];" << describe(delta));
            shared[key] = bytes;
         }
      }
      for (const auto& [key, direction] : activity) {
         BOOST_REQUIRE_MESSAGE(delta.contains(key), "missing bill [" << std::get<0>(key) << " " << std::get<1>(key)
                                                      << " table " << std::get<2>(key) << "];" << describe(delta));
      }
      return shared;
   }
   /// The `wireclaims` row of `account` and `issuer`; null when there is none.
   fc::variant wire_claim_of(name account, name issuer) {
      const auto row = find_kv_row("wireclaims"_n, wire_claim_key_of(account, issuer));
      return row ? decode(bond_abi_ser, "wire_claim", std::vector<char>(row->value.begin(), row->value.end()))
                 : fc::variant();
   }
   /// The kv key of the `wireclaims` row of `account` and `issuer`: both names as big-endian words.
   static std::string wire_claim_key_of(name account, name issuer) {
      std::string key;
      append_key_word(key, account.to_uint64_t());
      append_key_word(key, issuer.to_uint64_t());
      return key;
   }
   /// The kv key of the `bonds` row of `request_id` and `underwriter`.
   static std::string bond_key_of(uint64_t request_id, name underwriter) {
      std::string key = request_key_of(request_id);
      append_key_word(key, underwriter.to_uint64_t());
      return key;
   }
   /// The kv key of the `escrows` row of `request_id` and `kind`.
   static std::string escrow_key_of(uint64_t request_id, uint8_t kind) {
      std::string key = request_key_of(request_id);
      key.push_back(char(kind));
      return key;
   }
   /// Rewrite request `id`'s stored token code to `token_code`, bypassing the contract, to stand for a
   /// token that no longer resolves.
   void rewrite_request_token(uint64_t id, std::string_view token_code) {
      mvo row(request_row_of(id).get_object());
      row("token_code", codename_mvo(token_code));
      const auto bytes = bond_abi_ser.variant_to_binary("request_row", row,
                                                        fixture_abi_yield_function());
      const auto stored = find_kv_row("requests"_n, request_key_of(id));
      BOOST_REQUIRE(stored);
      const_cast<kv_store&>(control->kv()).update_row(*stored, stored->payer, {bytes.data(), bytes.size()});
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
                                              fixture_abi_yield_function());
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
                                              fixture_abi_yield_function());
         seen.push_back({v["custodian"].as<name>(), v["beneficiary"].as<name>(), v["quantity"].as<asset>(), {}});
      }
      return seen;
   }
   /// `claimwire` of `account`'s WIRE from `issuer`'s requests, signed by `account`.
   action_result claimwire(name account, name issuer) {
      return push_bond(account, "claimwire"_n, mvo()("account", account)("issuer", issuer));
   }
   /// The `sysio.liq::addyield` actions the last pushed transaction executed, as `(from, quantity)`.
   std::vector<std::pair<name, asset>> addyields() {
      std::vector<std::pair<name, asset>> seen;
      if (!last_trace) return seen;
      for (const auto& at : last_trace->action_traces) {
         if (at.act.account != LIQ_ACCOUNT || at.act.name != "addyield"_n || at.receiver != LIQ_ACCOUNT) continue;
         const auto v = liq_abi_ser.binary_to_variant("addyield", at.act.data,
                                                      fixture_abi_yield_function());
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

// A request's rows bill the issuer that created them: the request row and its statement index entry,
// the bounty escrow, and the issuer's `wireclaims` row for the WIRE its escrows will earn. The sysio RAM
// pool pays only for shared rows: on the first request, the id counter and sysio.bond's holder row of
// the token on sysio.liq.
BOOST_FIXTURE_TEST_CASE(request_rows_bill_the_issuer, sysio_bond_tester) try {
   constexpr auto                issuer = "issuer"_n;
   const std::map<bill_key, int> request_rows{
      {bill_of(issuer, BOND_ACCOUNT, "requests"_n), +1},
      {bill_of(issuer, BOND_ACCOUNT, "requests"_n, "bystatement"_n), +1},
      {bill_of(issuer, BOND_ACCOUNT, "escrows"_n), +1},
   };
   auto first_rows = request_rows;
   first_rows[bill_of(issuer, BOND_ACCOUNT, "wireclaims"_n)] = +1;
   const auto shared = require_billed(billed_by([&] {
      BOOST_REQUIRE_EQUAL(success(), request(issuer, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   }), first_rows);
   BOOST_REQUIRE_MESSAGE(shared.contains(bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "bondcounters"_n)), describe(shared));
   BOOST_REQUIRE_MESSAGE(shared.contains(bill_of(SYSIO_ACCOUNT, LIQ_ACCOUNT, "accounts"_n)), describe(shared));
   BOOST_REQUIRE_EQUAL(issuer, payer_of("requests"_n, request_key_of(1)));
   BOOST_REQUIRE_EQUAL(issuer, payer_of("escrows"_n, escrow_key_of(1, KIND_BOUNTY_BYTE)));
   BOOST_REQUIRE_EQUAL(issuer, payer_of("wireclaims"_n, wire_claim_key_of(issuer, issuer)));
   BOOST_REQUIRE_EQUAL(1u, wire_claim_of(issuer, issuer)["reservations"].as_uint64());

   // A second request bills the issuer its own rows and reserves the same balance row once more; with
   // the counter and the holder row in place, the sysio pool pays for nothing.
   BOOST_REQUIRE(require_billed(billed_by([&] {
      BOOST_REQUIRE_EQUAL(success(), request(issuer, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 1 * UNIT));
   }), request_rows).empty());
   BOOST_REQUIRE_EQUAL(2u, wire_claim_of(issuer, issuer)["reservations"].as_uint64());

   // A raise writes no new row; a WIRE request earns no WIRE, so it reserves no balance row.
   BOOST_REQUIRE(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), addbounty(issuer, 2, 1 * UNIT)); }).empty());
   require_billed(billed_by([&] {
      BOOST_REQUIRE_EQUAL(success(), request(issuer, "oppenvelope"_n, bytes(32, 'c'), WIRE, 10 * UNIT, 1 * UNIT));
   }), request_rows);
   BOOST_REQUIRE_EQUAL(2u, wire_claim_of(issuer, issuer)["reservations"].as_uint64());
} FC_LOG_AND_RETHROW()

// A privileged account -- a system contract, as sysio.synd is when it issues for its envelopes -- has
// no RAM quota of its own, so the rows of its request bill the sysio RAM pool instead.
BOOST_FIXTURE_TEST_CASE(a_privileged_issuer_bills_sysio, sysio_bond_tester) try {
   // sysio.tokens, the token registry, is a privileged system contract of the fixture.
   constexpr auto system_contract = TOKENS_ACCOUNT;
   BOOST_REQUIRE_EQUAL(success(), mint(system_contract, 100 * UNIT));

   require_billed(billed_by([&] {
      BOOST_REQUIRE_EQUAL(success(),
                          request(system_contract, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   }), {
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "requests"_n), +1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "requests"_n, "bystatement"_n), +1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "escrows"_n), +1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "wireclaims"_n), +1},
   });
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("requests"_n, request_key_of(1)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("escrows"_n, escrow_key_of(1, KIND_BOUNTY_BYTE)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("wireclaims"_n, wire_claim_key_of(system_contract, system_contract)));

   // A raise keeps the rows on the pool.
   BOOST_REQUIRE(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), addbounty(system_contract, 1, 1 * UNIT)); }).empty());
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("escrows"_n, escrow_key_of(1, KIND_BOUNTY_BYTE)));
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

// An underwriter's first bond is at least covered / MAX_BONDS_PER_REQUEST. A top-up of its own row, and
// a first bond that takes the whole remainder, may be smaller.
BOOST_FIXTURE_TEST_CASE(accept_refuses_a_first_bond_below_the_request_minimum, sysio_bond_tester) try {
   constexpr uint64_t increment = UNIT / 100;
   const auto         refused   = wasm_assert_msg(std::string(first_bond_msg));
   // 10 UNIT / 64 is 15.625 increments: the smallest first bond is 16.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const auto before = liq_balance("alice"_n);

   BOOST_REQUIRE_EQUAL(refused, accept("alice"_n, 1, 15 * increment));
   BOOST_REQUIRE_EQUAL(before, liq_balance("alice"_n));
   BOOST_REQUIRE(bond_rows_of(1).empty());
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 16 * increment));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, increment));
   BOOST_REQUIRE_EQUAL(17 * increment, bond_amount_of(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(refused, accept("bob"_n, 1, 15 * increment));

   // alice leaves 15 increments uncovered: bob's first bond takes all of it.
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT - 32 * increment));
   BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 15 * increment));
   BOOST_REQUIRE_EQUAL(15 * increment, bond_amount_of(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));
} FC_LOG_AND_RETHROW()

// Bonded at the minimum by as many underwriters as it admits, a request holds MAX_BONDS_PER_REQUEST
// rows, and one prune erases it with all of them.
BOOST_FIXTURE_TEST_CASE(a_request_holds_at_most_the_maximum_bonds, sysio_bond_tester) try {
   constexpr uint64_t increment  = UNIT / 100;
   constexpr uint64_t minimum    = 2 * increment;
   constexpr uint32_t window_sec = 3600;
   std::vector<name> underwriters;
   for (uint64_t i = 0; i < MAX_BONDS_PER_REQUEST; ++i)
      underwriters.push_back(name(std::string("uw") + char('a' + i / 8) + char('a' + i % 8)));
   create_accounts(underwriters);
   for (const name underwriter : underwriters) BOOST_REQUIRE_EQUAL(success(), mint(underwriter, minimum));

   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL,
                                          MAX_BONDS_PER_REQUEST * minimum, 0, window_sec));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(first_bond_msg)), accept("alice"_n, 1, increment));
   for (const name underwriter : underwriters) BOOST_REQUIRE_EQUAL(success(), accept(underwriter, 1, minimum));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));
   BOOST_REQUIRE_EQUAL(MAX_BONDS_PER_REQUEST, bond_rows_of(1).size());

   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   for (const name underwriter : underwriters) BOOST_REQUIRE_EQUAL(success(), claim(1, underwriter));
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 1));
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(bond_rows_of(1).empty());
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

// A bond on an ordinary issuer's request bills that issuer, never the underwriter: the bond row, and at
// the underwriter's first bond on a shadow-token request of this issuer its `wireclaims` row, which the
// claim paying the bond's WIRE only updates. A later bond by the same underwriter writes no new row.
BOOST_FIXTURE_TEST_CASE(accept_bills_the_issuer, sysio_bond_tester) try {
   constexpr auto issuer = "issuer"_n;
   BOOST_REQUIRE_EQUAL(success(), request(issuer, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   const std::map<bill_key, int> bond_rows{
      {bill_of(issuer, BOND_ACCOUNT, "bonds"_n), +1},
      {bill_of(issuer, BOND_ACCOUNT, "wireclaims"_n), +1},
   };
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 4 * UNIT)); }), bond_rows);
   BOOST_REQUIRE_EQUAL(issuer, payer_of("bonds"_n, bond_key_of(1, "alice"_n)));
   BOOST_REQUIRE_EQUAL(issuer, payer_of("requests"_n, request_key_of(1)));
   BOOST_REQUIRE_EQUAL(issuer, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, issuer)));
   BOOST_REQUIRE_EQUAL(1u, wire_claim_of("alice"_n, issuer)["reservations"].as_uint64());

   // A second bond by alice adds to her rows; bob's first bills the issuer like alice's.
   BOOST_REQUIRE(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 2 * UNIT)); }).empty());
   BOOST_REQUIRE_EQUAL(1u, wire_claim_of("alice"_n, issuer)["reservations"].as_uint64());
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), accept("bob"_n, 1, 4 * UNIT)); }), bond_rows);
   BOOST_REQUIRE_EQUAL(issuer, payer_of("wireclaims"_n, wire_claim_key_of("bob"_n, issuer)));

   // A bond on a WIRE request earns no WIRE, so it reserves no balance row.
   BOOST_REQUIRE_EQUAL(success(), request(issuer, "oppenvelope"_n, bytes(32, 'b'), WIRE, 10 * UNIT, 0));
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 2, 4 * UNIT)); }),
                  {{bill_of(issuer, BOND_ACCOUNT, "bonds"_n), +1}});
   BOOST_REQUIRE_EQUAL(1u, wire_claim_of("alice"_n, issuer)["reservations"].as_uint64());
} FC_LOG_AND_RETHROW()

// An accept the ordinary issuer has no RAM for fails, and the underwriter keeps its tokens.
BOOST_FIXTURE_TEST_CASE(accept_fails_when_the_issuer_has_no_ram, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   exhaust_ram("issuer"_n);
   const auto alice_balance = liq_balance("alice"_n);

   const auto result = accept("alice"_n, 1, 10 * UNIT);
   BOOST_REQUIRE_MESSAGE(result.find("account issuer has insufficient ram") != std::string::npos, result);
   BOOST_REQUIRE_EQUAL(0u, bond_amount_of(1, "alice"_n));
   BOOST_REQUIRE(wire_claim_of("alice"_n, "issuer"_n).is_null());
   BOOST_REQUIRE_EQUAL(alice_balance, liq_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// An accept on a privileged issuer's request -- a system contract's, as sysio.synd's are -- bills the
// bond row and the underwriter's balance row for that issuer to the sysio RAM pool.
BOOST_FIXTURE_TEST_CASE(accept_on_a_privileged_issuers_request_bills_sysio, sysio_bond_tester) try {
   // sysio.tokens, the token registry, is a privileged system contract of the fixture.
   constexpr auto system_contract = TOKENS_ACCOUNT;
   BOOST_REQUIRE_EQUAL(success(), request(system_contract, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));

   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT)); }), {
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "bonds"_n), +1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "wireclaims"_n), +1},
   });
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("bonds"_n, bond_key_of(1, "alice"_n)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("requests"_n, request_key_of(1)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, system_contract)));
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

// The hold-bond escrow is the challenge's row, so it bills the beneficiary, never the issuer: naming an
// account is no authority to charge it, so a hold without the beneficiary's signature is refused.
BOOST_FIXTURE_TEST_CASE(hold_bills_the_hold_bond_escrow_to_the_beneficiary, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT));
   const auto before = liq_balance("issuer"_n);

   BOOST_REQUIRE_EQUAL(error("missing authority of carol"), hold_without_beneficiary("issuer"_n, 1, "carol"_n));
   BOOST_REQUIRE_EQUAL(before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));

   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 1, "carol"_n)); }),
                  {{bill_of("carol"_n, BOND_ACCOUNT, "escrows"_n), +1}});
   BOOST_REQUIRE_EQUAL("carol"_n, payer_of("escrows"_n, escrow_key_of(1, KIND_HOLD_BOND_BYTE)));
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("escrows"_n, escrow_key_of(1, KIND_BOUNTY_BYTE)));
   // The issuer still funds the hold bond, and its escrow's WIRE is still the issuer's.
   BOOST_REQUIRE_EQUAL(before - 1 * UNIT, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL("issuer", escrow_row_of(1, KIND_HOLD_BOND_BYTE)["funder"].as_string());
} FC_LOG_AND_RETHROW()

// A hold the beneficiary has no RAM for fails, and the issuer keeps its tokens.
BOOST_FIXTURE_TEST_CASE(hold_fails_when_the_beneficiary_has_no_ram, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   exhaust_ram("carol"_n);
   const auto before = liq_balance("issuer"_n);

   const auto result = hold("issuer"_n, 1, "carol"_n);
   BOOST_REQUIRE_MESSAGE(result.find("account carol has insufficient ram") != std::string::npos, result);
   BOOST_REQUIRE_EQUAL(before, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE(escrow_row_of(1, KIND_HOLD_BOND_BYTE).is_null());
} FC_LOG_AND_RETHROW()

// An ordinary challenger pays for its hold even on a privileged issuer's request, whose own rows bill the
// sysio RAM pool; the prune that erases the ruled request returns each row's RAM to the account it billed.
BOOST_FIXTURE_TEST_CASE(an_ordinary_challenger_pays_for_its_hold_on_a_privileged_issuers_request,
                        sysio_bond_tester) try {
   // sysio.tokens, the token registry, is a privileged system contract of the fixture.
   constexpr auto system_contract = TOKENS_ACCOUNT;
   BOOST_REQUIRE_EQUAL(success(), mint(system_contract, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), request(system_contract, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   const auto carol_ram = ram_usage("carol"_n);

   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), hold(system_contract, 1, "carol"_n)); }),
                  {{bill_of("carol"_n, BOND_ACCOUNT, "escrows"_n), +1}});
   BOOST_REQUIRE_EQUAL("carol"_n, payer_of("escrows"_n, escrow_key_of(1, KIND_HOLD_BOND_BYTE)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("requests"_n, request_key_of(1)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("bonds"_n, bond_key_of(1, "alice"_n)));

   // INVALID: the challenger claims the hold bond, the issuer its forfeit; nothing is billed.
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));
   const auto carol_liq = liq_balance("carol"_n);
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claim(1, "carol"_n)); }), {});
   BOOST_REQUIRE_EQUAL(carol_liq + 1 * UNIT, liq_balance("carol"_n));
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claim(1, system_contract, "carol"_n)); }), {});

   pass_retention();
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), prune(0, 10)); }), {
      {bill_of("carol"_n, BOND_ACCOUNT, "escrows"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "requests"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "requests"_n, "bystatement"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "bonds"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "wireclaims"_n), -1},
   });
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE_EQUAL(carol_ram, ram_usage("carol"_n));
   BOOST_REQUIRE(wire_claim_of("alice"_n, system_contract).is_null());
   BOOST_REQUIRE(wire_claim_of(system_contract, system_contract).is_null());
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

   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 1));
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
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 1));
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
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 1));
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
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(held, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(success(), claim(2, "issuer"_n));
   BOOST_REQUIRE_EQUAL(held + 1 * UNIT, liq_balance("issuer"_n));
   BOOST_REQUIRE_EQUAL(0u, liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(2, "carol"_n));

   // In WIRE the bounty is claimed through sysio.token.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'c'), WIRE, 10 * UNIT, 1 * UNIT));
   const auto sysio_wire = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 3));
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
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));
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

   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));
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

BOOST_FIXTURE_TEST_CASE(rulings_require_the_contract_account_and_refuse_terminal_requests, sysio_bond_tester) try {
   constexpr uint32_t window_sec = 3600;
   bonded_request(1, 'a', window_sec);
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.bond"), rslvvalid("alice"_n, 1));
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.bond"), rslvinvalid("alice"_n, 1));
   // `sysio`'s own authority is not the contract's: governance rules through `sysio.bond`'s permissions.
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.bond"), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.bond"), rslvinvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), rslvvalid(BOND_ACCOUNT, 99));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(request_missing_msg)), rslvinvalid(BOND_ACCOUNT, 99));
   // Not terminal yet: nothing to claim.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_BONDED, request_state_of(1));

   // Approval is terminal: no ruling follows it.
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvvalid(BOND_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvinvalid(BOND_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));

   // Nor does a ruling follow a ruling.
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvinvalid(BOND_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvvalid(BOND_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'c'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 3));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(terminal_msg)), rslvvalid(BOND_ACCOUNT, 3));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(3));
} FC_LOG_AND_RETHROW()

/// Until a ruling is linked to its permission, only the contract account's own authority can rule:
/// before the permission exists, and while it exists but the action still requires `active`.
BOOST_FIXTURE_TEST_CASE(rulings_have_no_delegate_until_permissions_are_linked, sysio_bond_tester) try {
   open_request('a');
   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, config::active_name, 1), unsatisfied_authority_msg);
   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVVALID, 1), missing_permission_msg);
   require_refusal(rule_through(RSLVINVALID, SYSIO_ACCOUNT, RSLVINVALID, 1), missing_permission_msg);

   // The permissions exist and name the judge, but nothing is linked: a ruling still requires `active`.
   for (auto ruling : {RSLVVALID, RSLVINVALID}) delegate_ruling(ruling, {JUDGE_ACCOUNT, SYSIO_ACCOUNT});
   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVVALID, 1), irrelevant_authority_msg);
   require_refusal(rule_through(RSLVINVALID, SYSIO_ACCOUNT, RSLVINVALID, 1), irrelevant_authority_msg);
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));

   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, BOND_ACCOUNT, config::active_name, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
} FC_LOG_AND_RETHROW()

/// As deployed, `sysio.bond@active` delegates to `sysio@active`: governance rules through it with no
/// ruling permission at all, declaring `sysio.bond@active` rather than its own account.
BOOST_FIXTURE_TEST_CASE(governance_rules_through_the_contract_active_authority, sysio_bond_tester) try {
   for (char fill : {'a', 'b'}) open_request(fill);
   set_authority(BOND_ACCOUNT, config::active_name, delegated_authority({SYSIO_ACCOUNT}), config::owner_name);
   produce_block();

   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, config::active_name, 1), unsatisfied_authority_msg);
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.bond"), rslvvalid(SYSIO_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, SYSIO_ACCOUNT, config::active_name, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVINVALID, SYSIO_ACCOUNT, config::active_name, 2));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(2));
} FC_LOG_AND_RETHROW()

/// The judge and governance each rule through the linked permission; nobody else does, and the
/// permission of one ruling does not authorize the other.
BOOST_FIXTURE_TEST_CASE(linked_permissions_delegate_each_ruling_to_the_judge_and_governance, sysio_bond_tester) try {
   delegate_rulings();
   for (char fill : {'a', 'b', 'c', 'd', 'e'}) open_request(fill);

   // A stranger satisfies neither permission, and the judge's own account is not the contract's.
   require_refusal(rule_through(RSLVVALID, "alice"_n, RSLVVALID, 1), unsatisfied_authority_msg);
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.bond"), rslvvalid(JUDGE_ACCOUNT, 1));
   // Each permission authorizes its own ruling only.
   require_refusal(rule_through(RSLVINVALID, JUDGE_ACCOUNT, RSLVVALID, 1), irrelevant_authority_msg);
   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVINVALID, 1), irrelevant_authority_msg);
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));

   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVVALID, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVINVALID, JUDGE_ACCOUNT, RSLVINVALID, 2));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(2));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, SYSIO_ACCOUNT, RSLVVALID, 3));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(3));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVINVALID, SYSIO_ACCOUNT, RSLVINVALID, 4));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(4));
   // The parent `active` still satisfies a linked child permission.
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 5));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(5));

   // The delegation covers the rulings and nothing else: `setconfig` stays `sysio`'s.
   require_refusal(sysio_system::test_support::push_contract_action_and_produce_block(
                      *this, BOND_ACCOUNT, bond_abi_ser, JUDGE_ACCOUNT, "setconfig"_n, mvo()("hold_bps", 500),
                      last_trace, {{BOND_ACCOUNT, RSLVVALID}}),
                   irrelevant_authority_msg);
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), setconfig(JUDGE_ACCOUNT, 500));
} FC_LOG_AND_RETHROW()

/// Native updateauth replaces or removes a delegate, and gives the two rulings different authorities,
/// without touching the contract.
BOOST_FIXTURE_TEST_CASE(updateauth_replaces_the_judge_and_separates_the_rulings, sysio_bond_tester) try {
   delegate_rulings();
   for (char fill : {'a', 'b', 'c'}) open_request(fill);

   // Replace the judge on VALID; leave INVALID to governance alone.
   delegate_ruling(RSLVVALID, {"alice"_n, SYSIO_ACCOUNT});
   delegate_ruling(RSLVINVALID, {SYSIO_ACCOUNT});
   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVVALID, 1), unsatisfied_authority_msg);
   require_refusal(rule_through(RSLVINVALID, JUDGE_ACCOUNT, RSLVINVALID, 1), unsatisfied_authority_msg);
   require_refusal(rule_through(RSLVINVALID, "alice"_n, RSLVINVALID, 1), unsatisfied_authority_msg);
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, "alice"_n, RSLVVALID, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVINVALID, SYSIO_ACCOUNT, RSLVINVALID, 2));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(2));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(3));
} FC_LOG_AND_RETHROW()

/// Under the native rules a permission may rewrite itself: the judge can change the authority of the
/// ruling permission it satisfies, though not the other one. The contract's `active` reverts it.
BOOST_FIXTURE_TEST_CASE(a_delegate_can_rewrite_its_own_ruling_permission_and_active_reverts_it, sysio_bond_tester) try {
   delegate_rulings();
   for (char fill : {'a', 'b', 'c'}) open_request(fill);

   // A stranger cannot, and the judge's hold on VALID does not reach an authority it does not satisfy.
   require_refusal(rewrite_ruling_permission("alice"_n, RSLVVALID, {"alice"_n}), unsatisfied_authority_msg);
   delegate_ruling(RSLVINVALID, {SYSIO_ACCOUNT});
   require_refusal(rewrite_ruling_permission(JUDGE_ACCOUNT, RSLVINVALID, {JUDGE_ACCOUNT}), unsatisfied_authority_msg);

   // The judge hands VALID to alice alone, dropping governance from the permission's own list.
   BOOST_REQUIRE_EQUAL(success(), rewrite_ruling_permission(JUDGE_ACCOUNT, RSLVVALID, {"alice"_n}));
   require_refusal(rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVVALID, 1), unsatisfied_authority_msg);
   require_refusal(rule_through(RSLVVALID, SYSIO_ACCOUNT, RSLVVALID, 1), unsatisfied_authority_msg);
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, "alice"_n, RSLVVALID, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));

   // The parent `active` still rules, and puts the permission back.
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(2));
   delegate_ruling(RSLVVALID, {JUDGE_ACCOUNT, SYSIO_ACCOUNT});
   require_refusal(rule_through(RSLVVALID, "alice"_n, RSLVVALID, 3), unsatisfied_authority_msg);
   BOOST_REQUIRE_EQUAL(success(), rule_through(RSLVVALID, JUDGE_ACCOUNT, RSLVVALID, 3));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(3));
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
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 1));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(1));
   BOOST_REQUIRE(settlements().empty());

   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'b'), LIQX, 1, 0));
   BOOST_REQUIRE_EQUAL(success(), hold("issuer"_n, 2, "carol"_n));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 2));
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
   BOOST_REQUIRE_EQUAL(success(), rslvvalid(BOND_ACCOUNT, 2));
   BOOST_REQUIRE_EQUAL(STATE_VALID, request_state_of(2));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 3));
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
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   BOOST_REQUIRE(!request_row_of(1).is_null());
   BOOST_REQUIRE_EQUAL(residual, raw_bond_of(1, "alice"_n).yield.owed_wire);
   // Unattributed custody earns slack that can back the already-recorded residual.
   BOOST_REQUIRE_EQUAL(success(), mint(BOND_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, UNIT));
   BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n));
   BOOST_REQUIRE_EQUAL(principal_before + 10 * UNIT, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, raw_bond_of(1, "alice"_n).yield.owed_wire);
   const auto wire_before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, "issuer"_n));
   BOOST_REQUIRE_EQUAL(wire_before + earned, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
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
   BOOST_REQUIRE(claimwire("alice"_n, "issuer"_n) != success());
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), prune(0, 10));
   set_code("alice"_n, std::vector<uint8_t>{});
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, "issuer"_n));
   BOOST_REQUIRE_EQUAL(before_wire + earned, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claimwire("alice"_n, "issuer"_n));
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
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, "issuer"_n));
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
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, "issuer"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + at_claim, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, liq_owed(BOND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// An ordinary issuer pays for everything its request causes, and gets all of it back: the WIRE balance
// rows are reserved at `request` and `accept`, so the claims that record WIRE on them and the withdrawals
// need no RAM even from an issuer that has none left, and `prune` and `claimwire` return every byte.
BOOST_FIXTURE_TEST_CASE(an_ordinary_issuer_pays_for_its_request_through_its_lifecycle, sysio_bond_tester) try {
   constexpr auto     issuer     = "issuer"_n;
   constexpr uint32_t window_sec = 3600;
   const auto         issuer_ram = ram_usage(issuer);
   const auto         alice_ram  = ram_usage("alice"_n);

   BOOST_REQUIRE_EQUAL(success(), request(issuer, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 1 * UNIT,
                                          window_sec));
   BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, 1, 10 * UNIT));
   BOOST_REQUIRE_GT(ram_usage(issuer), issuer_ram);
   BOOST_REQUIRE_EQUAL(alice_ram, ram_usage("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   pass_window(window_sec);
   BOOST_REQUIRE_EQUAL(success(), approve(1));

   // The issuer has no RAM left: the claims still record WIRE on the rows reserved for it.
   exhaust_ram(issuer);
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claim(1, "alice"_n)); }), {});
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claim(1, issuer)); }), {});
   const uint64_t alice_owed  = wire_claim_of("alice"_n, issuer)["amount"].as_uint64();
   const uint64_t issuer_owed = wire_claim_of(issuer, issuer)["amount"].as_uint64();
   BOOST_REQUIRE_GT(alice_owed, 0u);
   BOOST_REQUIRE_GT(issuer_owed, 0u);

   // alice withdraws while her bond row still reserves the balance: it stays, at zero, for later yield.
   const auto alice_wire = wire_balance("alice"_n);
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, issuer)); }), {});
   BOOST_REQUIRE_EQUAL(alice_wire + alice_owed, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, wire_claim_of("alice"_n, issuer)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, wire_claim_of("alice"_n, issuer)["reservations"].as_uint64());

   // The prune returns the request's rows to the issuer. alice's balance row goes with her bond; the
   // issuer's own is still owed WIRE, so it stays, reserved by nothing, until withdrawn.
   pass_retention();
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), prune(0, 10)); }), {
      {bill_of(issuer, BOND_ACCOUNT, "requests"_n), -1},
      {bill_of(issuer, BOND_ACCOUNT, "requests"_n, "bystatement"_n), -1},
      {bill_of(issuer, BOND_ACCOUNT, "escrows"_n), -1},
      {bill_of(issuer, BOND_ACCOUNT, "bonds"_n), -1},
      {bill_of(issuer, BOND_ACCOUNT, "wireclaims"_n), -1},
   });
   BOOST_REQUIRE(request_row_of(1).is_null());
   BOOST_REQUIRE(wire_claim_of("alice"_n, issuer).is_null());
   BOOST_REQUIRE_EQUAL(0u, wire_claim_of(issuer, issuer)["reservations"].as_uint64());

   const auto issuer_wire = wire_balance(issuer);
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claimwire(issuer, issuer)); }),
                  {{bill_of(issuer, BOND_ACCOUNT, "wireclaims"_n), -1}});
   BOOST_REQUIRE_EQUAL(issuer_wire + issuer_owed, wire_balance(issuer));
   BOOST_REQUIRE(wire_claim_of(issuer, issuer).is_null());
   BOOST_REQUIRE_EQUAL(issuer_ram, ram_usage(issuer));
   BOOST_REQUIRE_EQUAL(alice_ram, ram_usage("alice"_n));
} FC_LOG_AND_RETHROW()

// One account's WIRE from several issuers sits on one row per issuer, each billed like that issuer's
// requests: an ordinary issuer pays for its own, a privileged issuer's bills the sysio RAM pool. A later
// issuer's request never takes over another's row, a withdrawal pays one issuer's balance only, and each
// row's RAM returns to the issuer that paid for it.
BOOST_FIXTURE_TEST_CASE(wire_claims_from_several_issuers_stay_with_each_issuer, sysio_bond_tester) try {
   constexpr auto     system_contract = TOKENS_ACCOUNT;   // privileged
   constexpr uint32_t window_sec      = 3600;
   const auto         issuer_ram      = ram_usage("issuer"_n);
   const auto         bob_ram         = ram_usage("bob"_n);
   const auto         alice_ram       = ram_usage("alice"_n);
   const auto         request_by      = [&](name issuer, uint64_t id, char fill) {
      BOOST_REQUIRE_EQUAL(success(),
                          request(issuer, "oppenvelope"_n, bytes(32, fill), LIQSOL, 10 * UNIT, 0, window_sec));
      BOOST_REQUIRE_EQUAL(success(), accept("alice"_n, id, 10 * UNIT));
   };
   request_by("issuer"_n, 1, 'a');
   request_by(system_contract, 2, 'b');
   request_by("bob"_n, 3, 'c');
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, "issuer"_n)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, system_contract)));
   BOOST_REQUIRE_EQUAL("bob"_n, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, "bob"_n)));
   BOOST_REQUIRE_EQUAL(alice_ram, ram_usage("alice"_n));

   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   pass_window(window_sec);
   for (uint64_t id = 1; id <= 3; ++id) BOOST_REQUIRE_EQUAL(success(), approve(id));
   // Each claim records on its own issuer's row and bills no one.
   for (uint64_t id : {3, 2, 1})
      require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claim(id, "alice"_n)); }), {});
   const uint64_t from_issuer = wire_claim_of("alice"_n, "issuer"_n)["amount"].as_uint64();
   const uint64_t from_system = wire_claim_of("alice"_n, system_contract)["amount"].as_uint64();
   const uint64_t from_bob    = wire_claim_of("alice"_n, "bob"_n)["amount"].as_uint64();
   BOOST_REQUIRE_GT(from_issuer, 0u);
   BOOST_REQUIRE_GT(from_system, 0u);
   BOOST_REQUIRE_GT(from_bob, 0u);
   BOOST_REQUIRE_EQUAL("issuer"_n, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, "issuer"_n)));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, payer_of("wireclaims"_n, wire_claim_key_of("alice"_n, system_contract)));

   // A withdrawal pays one issuer's balance, and only that one.
   const auto alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, "bob"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + from_bob, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(from_issuer, wire_claim_of("alice"_n, "issuer"_n)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(from_system, wire_claim_of("alice"_n, system_contract)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claimwire("alice"_n, "bob"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(nothing_msg)), claimwire("alice"_n, "carol"_n));

   // The prune returns each issuer its own rows; the balances still owed stay until withdrawn.
   pass_retention();
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), prune(0, 10)); }), {
      {bill_of("issuer"_n, BOND_ACCOUNT, "requests"_n), -1},
      {bill_of("issuer"_n, BOND_ACCOUNT, "requests"_n, "bystatement"_n), -1},
      {bill_of("issuer"_n, BOND_ACCOUNT, "bonds"_n), -1},
      {bill_of("issuer"_n, BOND_ACCOUNT, "wireclaims"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "requests"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "requests"_n, "bystatement"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "bonds"_n), -1},
      {bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "wireclaims"_n), -1},
      {bill_of("bob"_n, BOND_ACCOUNT, "requests"_n), -1},
      {bill_of("bob"_n, BOND_ACCOUNT, "requests"_n, "bystatement"_n), -1},
      {bill_of("bob"_n, BOND_ACCOUNT, "bonds"_n), -1},
      {bill_of("bob"_n, BOND_ACCOUNT, "wireclaims"_n), -1},
   });
   BOOST_REQUIRE(wire_claim_of("alice"_n, "bob"_n).is_null());
   BOOST_REQUIRE_EQUAL(bob_ram, ram_usage("bob"_n));
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, "issuer"_n)); }),
                  {{bill_of("issuer"_n, BOND_ACCOUNT, "wireclaims"_n), -1}});
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n, system_contract)); }),
                  {{bill_of(SYSIO_ACCOUNT, BOND_ACCOUNT, "wireclaims"_n), -1}});
   BOOST_REQUIRE_EQUAL(alice_wire + from_bob + from_issuer + from_system, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(issuer_ram, ram_usage("issuer"_n));
   BOOST_REQUIRE_EQUAL(alice_ram, ram_usage("alice"_n));
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
   BOOST_REQUIRE_EQUAL(success(), claimwire("issuer"_n, "issuer"_n));
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
   BOOST_REQUIRE_EQUAL(success(), claimwire("issuer"_n, "issuer"_n));
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
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));
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
   BOOST_REQUIRE_EQUAL(success(), claimwire("issuer"_n, "issuer"_n));
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
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));

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
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 3));
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
   for (uint64_t id = 1; id <= 3; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, id));
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
   for (uint64_t id = 1; id <= 3; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, id));
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

// `limit` counts every request a call examines, so a run of requests it cannot prune -- open, still
// retained, or in a token that no longer resolves -- ends the call instead of being walked past.
BOOST_FIXTURE_TEST_CASE(prune_limit_counts_every_request_it_examines, sysio_bond_tester) try {
   for (char fill : {'a', 'b', 'c', 'd', 'e', 'f'}) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, fill), LIQSOL, 10 * UNIT, 0));
   }
   for (uint64_t id = 4; id <= 6; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, id));
   pass_retention();
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 3));
   rewrite_request_token(4, "NOPE");
   produce_block();
   // 1 and 2 are OPEN, 3 is retained, 4's token no longer resolves; 5 and 6 are prunable.

   BOOST_REQUIRE_EQUAL(success(), prune(0, 0));
   BOOST_REQUIRE_EQUAL(success(), prune(0, 4));
   for (uint64_t id = 1; id <= 6; ++id) BOOST_REQUIRE(!request_row_of(id).is_null());

   BOOST_REQUIRE_EQUAL(success(), prune(5, 4));
   BOOST_REQUIRE(request_row_of(5).is_null());
   BOOST_REQUIRE(request_row_of(6).is_null());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(1));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(2));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(3));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(4));
} FC_LOG_AND_RETHROW()

// A caller cannot have one call walk more than MAX_PRUNE_LIMIT requests.
BOOST_FIXTURE_TEST_CASE(prune_refuses_a_limit_above_the_maximum, sysio_bond_tester) try {
   BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, 'a'), LIQSOL, 10 * UNIT, 0));
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));
   pass_retention();

   const auto refused = wasm_assert_msg(std::string(prune_limit_msg));
   BOOST_REQUIRE_EQUAL(refused, prune(0, MAX_PRUNE_LIMIT + 1));
   BOOST_REQUIRE_EQUAL(refused, prune(0, std::numeric_limits<uint32_t>::max()));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, request_state_of(1));
   BOOST_REQUIRE_EQUAL(success(), prune(0, MAX_PRUNE_LIMIT));
   BOOST_REQUIRE(request_row_of(1).is_null());
} FC_LOG_AND_RETHROW()

// A crank that starts each call at the id after the requests the last one examined, and over at 0 once a
// call reaches the end, prunes every prunable request in one pass of ceil(rows / limit) calls.
BOOST_FIXTURE_TEST_CASE(a_crank_resuming_after_each_prune_prunes_every_prunable_request, sysio_bond_tester) try {
   constexpr uint32_t limit = 2;
   constexpr uint64_t rows  = 9;
   const auto         stays_open = [](uint64_t id) { return id % 3 == 0; };
   for (uint64_t id = 1; id <= rows; ++id) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, char('a' + id)), LIQSOL,
                                             10 * UNIT, 0));
      if (!stays_open(id)) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, id));
   }
   pass_retention();

   uint64_t from  = 0;
   uint32_t calls = 0;
   do {
      const auto ids = request_ids_from(from, limit + 1);
      BOOST_REQUIRE_EQUAL(success(), prune(from, limit));
      ++calls;
      from = ids.size() > limit ? ids[limit] : 0;
   } while (from != 0);

   BOOST_REQUIRE_EQUAL((rows + limit - 1) / limit, calls);
   for (uint64_t id = 1; id <= rows; ++id) {
      if (stays_open(id)) BOOST_REQUIRE_EQUAL(STATE_OPEN, request_state_of(id));
      else BOOST_REQUIRE(request_row_of(id).is_null());
   }
} FC_LOG_AND_RETHROW()

// A request whose token no longer resolves cannot be settled: prune skips it instead of failing,
// and still erases the prunable requests after it.
BOOST_FIXTURE_TEST_CASE(prune_skips_a_request_whose_token_no_longer_resolves, sysio_bond_tester) try {
   for (char fill : {'a', 'b'}) {
      BOOST_REQUIRE_EQUAL(success(), request("issuer"_n, "oppenvelope"_n, bytes(32, fill), LIQSOL, 10 * UNIT, 0));
   }
   for (uint64_t id = 1; id <= 2; ++id) BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, id));
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
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));

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
   BOOST_REQUIRE_EQUAL(success(), rslvinvalid(BOND_ACCOUNT, 1));
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
// payout holds no other underwriter's row in RAM, and the unpaid rows claim exactly as before.
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
   // The issuer gets back the RAM of alice's bond row and of her balance row, which owes nothing.
   require_billed(billed_by([&] { BOOST_REQUIRE_EQUAL(success(), prune(0, 10)); }), {
      {bill_of("issuer"_n, BOND_ACCOUNT, "bonds"_n), -1},
      {bill_of("issuer"_n, BOND_ACCOUNT, "wireclaims"_n), -1},
   });
   BOOST_REQUIRE_EQUAL(1u, bond_rows_of(1).size());
   BOOST_REQUIRE_EQUAL(6 * UNIT, bond_amount_of(1, "bob"_n));
   BOOST_REQUIRE_EQUAL(STATE_APPROVED, request_state_of(1));
   BOOST_REQUIRE(wire_claim_of("alice"_n, "issuer"_n).is_null());
   BOOST_REQUIRE_EQUAL(1u, wire_claim_of("bob"_n, "issuer"_n)["reservations"].as_uint64());
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
