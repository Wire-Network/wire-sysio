#pragma once

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/types/types.pb.h>
#include <sysio/testing/tester.hpp>

#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/exception/exception.hpp>
#include <fc/slug_name.hpp>
#include <fc/variant_object.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sysio_system::test_support {

using namespace sysio::chain;

/// Load an account's deployed ABI into a serializer used by contract integration fixtures.
template <typename Tester>
void load_account_abi(Tester& tester, name account, abi_serializer& out_ser) {
   const auto* metadata = tester.control->find_account_metadata(account);
   BOOST_REQUIRE(metadata != nullptr);
   abi_def parsed;
   BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(metadata->abi, parsed), true);
   out_ser.set_abi(std::move(parsed), Tester::fixture_abi_yield_function());
}

/// Store `encoded` as the kv row `id` of `code`'s `table` without going through the contract,
/// for state no action can reach. A new row is billed to `sysio`.
template <typename Tester>
void write_kv_row(Tester& tester, name code, name table, uint64_t id, const std::vector<char>& encoded) {
   char key[kv_pri_key_size];
   kv_encode_be64(key, id);
   auto& store = const_cast<kv_store&>(tester.control->kv());
   const auto table_id = compute_table_id(table.value);
   const std::string_view key_view(key, sizeof(key));
   const std::string_view value(encoded.data(), encoded.size());
   if (const auto row = store.find_row(code, table_id, key_view)) store.update_row(*row, row->payer, value);
   else store.insert_row(code, table_id, config::system_account_name, key_view, value);
}

/// Every row of `code`'s `table`, decoded as `abi_type`, in primary-key order across every scope.
template <typename Tester>
std::vector<fc::variant> table_rows(Tester& tester, name code, name table, const abi_serializer& abi,
                                    const char* abi_type) {
   const auto               table_id = compute_table_id(table.to_uint64_t());
   std::vector<fc::variant> rows;
   auto                     cursor = tester.control->kv().open_row_cursor();
   kv_row                   row;
   for (bool more = cursor->seek_lower({code, table_id}, row);
        more && row.code == code && row.table_id == table_id; more = cursor->next(row)) {
      const std::vector<char> raw(row.value.begin(), row.value.end());
      rows.push_back(abi.binary_to_variant(abi_type, raw, Tester::fixture_abi_yield_function()));
   }
   return rows;
}

/// The authorization an action needs when an unprivileged contract bills RAM to `signer`
/// (a deposit row emplaced for a user, say): the signer's `sysio.payer` beside `active`.
/// The active key satisfies both.
inline std::vector<permission_level> payer_authorization(name signer) {
   return {{signer, config::sysio_payer_name}, {signer, config::active_name}};
}

/// Build and sign one ABI-encoded contract-action transaction. `authorization` defaults to
/// the signer's `active`; pass `payer_authorization(signer)` when the action bills RAM.
template <typename Tester>
signed_transaction create_contract_action_transaction(Tester& tester, name contract, abi_serializer& serializer,
                                                       name signer, name action_name,
                                                       const fc::variant_object& data,
                                                       std::vector<permission_level> authorization = {}) {
   action act;
   act.account = contract;
   act.name = action_name;
   act.data = serializer.variant_to_binary(
      serializer.get_action_type(action_name), data,
      Tester::fixture_abi_yield_function());
   act.authorization = authorization.empty() ? std::vector<permission_level>{{signer, config::active_name}}
                                             : std::move(authorization);

   signed_transaction trx;
   trx.actions.emplace_back(std::move(act));
   tester.set_transaction_headers(trx);
   trx.sign(tester.get_private_key(signer, "active"), tester.control->get_chain_id());
   return trx;
}

/// Push one ABI-encoded action and retain its trace and console output.
template <typename Tester>
transaction_trace_ptr push_contract_action_trace(Tester& tester, name contract, abi_serializer& serializer,
                                                 name signer, name action_name,
                                                 const fc::variant_object& data,
                                                 std::vector<permission_level> authorization = {}) {
   auto trx = create_contract_action_transaction(tester, contract, serializer, signer, action_name, data,
                                                 std::move(authorization));
   return tester.push_transaction(trx);
}

/// Push one ABI-encoded action without advancing the head block.
template <typename Tester>
typename Tester::action_result push_contract_action(Tester& tester, name contract, abi_serializer& serializer,
                                                    name signer, name action_name,
                                                    const fc::variant_object& data) {
   try {
      push_contract_action_trace(tester, contract, serializer, signer, action_name, data);
      return Tester::success();
   } catch (const fc::exception& ex) {
      return Tester::error(ex.top_message());
   }
}

/// Push one ABI-encoded action and land it in its own block for stable TaPoS, handing back its
/// trace in `trace` (null after a failure) so a test can inspect inline actions and notifications.
template <typename Tester>
typename Tester::action_result push_contract_action_and_produce_block(
   Tester& tester, name contract, abi_serializer& serializer, name signer, name action_name,
   const fc::variant_object& data, transaction_trace_ptr& trace, std::vector<permission_level> authorization = {}) {
   trace.reset();
   try {
      trace = push_contract_action_trace(tester, contract, serializer, signer, action_name, data,
                                         std::move(authorization));
      tester.produce_block();
      return Tester::success();
   } catch (const fc::exception& ex) {
      trace.reset();
      return Tester::error(ex.top_message());
   }
}

/// Push one ABI-encoded action and land it in its own block for stable TaPoS.
template <typename Tester>
typename Tester::action_result push_contract_action_and_produce_block(
   Tester& tester, name contract, abi_serializer& serializer, name signer, name action_name,
   const fc::variant_object& data, std::vector<permission_level> authorization = {}) {
   transaction_trace_ptr trace;
   return push_contract_action_and_produce_block(tester, contract, serializer, signer, action_name, data, trace,
                                                 std::move(authorization));
}

// ---------------------------------------------------------------------------
//  Native permission delegation
//
//  A system contract delegates an action through a native child permission named after it and a
//  `linkauth`. These act on such a permission the way a delegate does, and name the chain's own
//  refusals so a negative test asserts why it was refused.
// ---------------------------------------------------------------------------

// The authorization manager's refusals of a declared authority, as it words them.
inline constexpr std::string_view unsatisfied_authority_msg = "but does not have signatures for it";
inline constexpr std::string_view irrelevant_authority_msg  = "declares irrelevant authority";
inline constexpr std::string_view missing_permission_msg    = "authorizations include a non-existent permission";

/// The authority any one of `delegates` satisfies alone through its `active`, in chain name order.
inline authority delegated_authority(std::vector<name> delegates) {
   std::sort(delegates.begin(), delegates.end());
   authority auth{1, {}, {}};
   for (auto delegate : delegates) auth.accounts.push_back({{delegate, config::active_name}, 1});
   return auth;
}

/// Push one native permission action (`updateauth`, `linkauth`, ...) declared by `account@declared`
/// and signed with `signer`'s active key, in its own block: what a delegate can do to the account
/// through the permission it satisfies.
template <typename Tester, typename NativeAction>
typename Tester::action_result push_native_action_and_produce_block(Tester& tester, name account, name declared,
                                                                    name signer, NativeAction&& native_action) {
   try {
      signed_transaction trx;
      trx.actions.emplace_back(std::vector<permission_level>{{account, declared}},
                               std::forward<NativeAction>(native_action));
      tester.set_transaction_headers(trx);
      trx.sign(tester.get_private_key(signer, "active"), tester.control->get_chain_id());
      tester.push_transaction(trx);
      tester.produce_block();
      return Tester::success();
   } catch (const fc::exception& ex) {
      return Tester::error(ex.top_message());
   }
}

/// The single transaction that hands each of `actions` to `auth`, as the emergency-stop playbook
/// prescribes: an `updateauth` of the permission named after each action, then per action an
/// `unlinkauth` and a `linkauth` back to that permission, all declared by `account@owner`.
/// `relink_only` names actions with no link to undo, which get no `unlinkauth`.
template <typename Tester>
typename Tester::action_result rotate_delegate(Tester& tester, name account, const std::vector<name>& actions,
                                              const authority& auth, const std::vector<name>& relink_only = {}) {
   try {
      const std::vector<permission_level> owner{{account, config::owner_name}};
      signed_transaction trx;
      for (auto action_name : actions)
         trx.actions.emplace_back(owner, updateauth{account, action_name, config::active_name, auth});
      for (auto action_name : actions) {
         if (std::find(relink_only.begin(), relink_only.end(), action_name) == relink_only.end())
            trx.actions.emplace_back(owner, unlinkauth{account, account, action_name});
         trx.actions.emplace_back(owner, linkauth{account, account, action_name, action_name});
      }
      tester.set_transaction_headers(trx);
      trx.sign(tester.get_private_key(account, "owner"), tester.control->get_chain_id());
      tester.push_transaction(trx);
      tester.produce_block();
      return Tester::success();
   } catch (const fc::exception& ex) {
      return Tester::error(ex.top_message());
   }
}

/// Where a delegate can leave the link of the action it was delegated before it is rotated out.
enum class delegated_link {
   in_place,       ///< linked to the permission of its own name, as delegated
   moved_beneath,  ///< linked to a permission the delegate added beneath that one
   any,            ///< linked to `sysio.any`, which any permission of the account satisfies
   owner,          ///< linked to `owner`, which a write declared `active` cannot change
   absent          ///< unlinked, so the action requires `active`
};

/// The refusal of an `unlinkauth` for an action that has no link, when pushed directly. Inside a
/// privileged sender such as `sysio.msig` the same unlink fails later, with "no link found".
inline constexpr std::string_view no_link_msg = "cannot unlink non-existent permission link";

/// Require `result` to be a refusal whose message contains `fragment`.
inline void require_refusal(const std::string& result, std::string_view fragment) {
   BOOST_REQUIRE_MESSAGE(!result.empty() && result.find(fragment) != std::string::npos,
                         "expected a refusal containing '" << fragment << "', got: " << result);
}

// ---------------------------------------------------------------------------
//  sysio.chains::outpost_addrs builders
//
//  `regchain` and `setoutpost` both take the remote outpost contract identities
//  as one nested struct. These build its variant form so a test states only the
//  addresses it cares about; the contract validates the set against the row's
//  ChainKind (see validate_outpost_addrs in sysio.chains.cpp).
// ---------------------------------------------------------------------------

/// The variant form of a `slug_name` action argument (`chain_code`, `token_code`,
/// `code`, ...): the packed value under the struct's one field.
inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return fc::mutable_variant_object()("value", fc::slug_name{ s }.value);
}

/// Every field empty — a chain registered before its remote contracts exist.
/// Valid for any kind; the batch-operator relay fails closed and skips such a row.
inline fc::mutable_variant_object no_outpost_mvo() {
   return fc::mutable_variant_object()
      ("opp_addr",         std::string{})
      ("opp_inbound_addr", std::string{});
}

/// EVM form: the OPP and OPPInbound contracts, each a 0x-prefixed 20-byte hex address.
inline fc::mutable_variant_object evm_outpost_mvo(std::string_view opp, std::string_view opp_inbound) {
   return fc::mutable_variant_object()
      ("opp_addr",         std::string{opp})
      ("opp_inbound_addr", std::string{opp_inbound});
}

/// SVM form: one base58 program id sends and receives envelopes, so
/// `opp_inbound_addr` must stay empty — the contract rejects a set that fills it in.
inline fc::mutable_variant_object svm_outpost_mvo(std::string_view program_id) {
   return fc::mutable_variant_object()
      ("opp_addr",         std::string{program_id})
      ("opp_inbound_addr", std::string{});
}

/// The user-link signature domain for EM (hex compressed key) and ED (base58
/// raw key), followed by account, chain kind, nonce and the fixed suffix.
inline std::string build_link_message(const fc::crypto::public_key& pub_key, const std::string& account,
                                      sysio::opp::types::ChainKind chain_kind, uint64_t nonce) {
   std::string pub_key_str;
   if (pub_key.type() == fc::crypto::public_key::key_type::ed) {
      const auto raw = pub_key.get<fc::crypto::ed::public_key_shim>().serialize();
      pub_key_str = "PUB_ED_" + fc::to_base58(reinterpret_cast<const char*>(raw.data()), raw.size(), [] {});
   } else {
      const auto compressed = pub_key.get<fc::em::public_key_shim>().serialize();
      pub_key_str = "PUB_EM_" + fc::to_hex(compressed.data(), compressed.size());
   }
   return pub_key_str + "|" + account + "|" + std::to_string(magic_enum::enum_integer(chain_kind)) + "|" +
          std::to_string(nonce) + "|createlink auth";
}

/// Sign the contract's curve-specific digest: EM uses keccak/EIP-191; ED uses
/// SHA-256 mapped to printable ASCII before the ED digest signing operation.
inline fc::crypto::signature sign_createlink(const fc::crypto::private_key& priv, const std::string& account,
                                             sysio::opp::types::ChainKind chain_kind, uint64_t nonce) {
   const auto message = build_link_message(priv.get_public_key(), account, chain_kind, nonce);
   if (priv.get_public_key().type() == fc::crypto::public_key::key_type::ed) {
      constexpr unsigned char PrintableStart = 33, PrintableCount = 94;
      const auto raw = fc::sha256::hash(message);
      std::array<char, 32> mapped;
      for (size_t i = 0; i < mapped.size(); ++i)
         mapped[i] = char((static_cast<unsigned char>(raw.data()[i]) % PrintableCount) + PrintableStart);
      return priv.sign(fc::sha256(mapped.data(), mapped.size()));
   }
   const auto msg_hash = fc::crypto::keccak256::hash(message);
   return priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));
}

// ---------------------------------------------------------------------------
//  Served chains: the sysio.authex links and sysio.opreg memberships a batch operator needs before
//  a chain's schedule seats it
// ---------------------------------------------------------------------------

namespace served_chains {

inline constexpr auto authex_account     = "sysio.authex"_n;
inline constexpr auto opreg_account      = "sysio.opreg"_n;
inline constexpr auto createlink_action  = "createlink"_n;
inline constexpr auto joinchain_action   = "joinchain"_n;
inline constexpr auto leavechain_action  = "leavechain"_n;
inline constexpr auto membership_table   = "servchains"_n;
inline constexpr auto membership_type    = "served_chain";

/// `sysio.opreg`'s `chain_service_status` names, as a decoded `servchains` row spells them.
namespace status {
inline constexpr std::string_view serving = "SERVING";
inline constexpr std::string_view left    = "LEFT";
inline constexpr std::string_view evicted = "EVICTED";
} // namespace status

/// Link `account` to a freshly generated key of `kind`'s curve (EM for EVM, ED for SVM) through its own
/// signed `sysio.authex::createlink`. Needs sysio.authex and a privileged sysio.dclaim deployed.
template <typename Tester>
void link_fresh_key(Tester& tester, name account, sysio::opp::types::ChainKind kind) {
   const fc::crypto::private_key priv = fc::crypto::private_key::generate(
      kind == sysio::opp::types::ChainKind::CHAIN_KIND_SVM ? fc::crypto::private_key::key_type::ed
                                                           : fc::crypto::private_key::key_type::em);
   const uint64_t nonce = tester.control->head().block_time().time_since_epoch().count() / 1000;
   tester.push_action(authex_account, createlink_action, account, fc::mutable_variant_object()
      ("chain_kind", kind)
      ("account",    account)
      ("sig",        sign_createlink(priv, account.to_string(), kind, nonce))
      ("pub_key",    priv.get_public_key())
      ("nonce",      nonce));
}

/// Push `sysio.opreg::joinchain(account, chain_code)` signed by `signer`: `sysio.opreg` (the bootstrap
/// path, the default) or the operator itself.
template <typename Tester>
typename Tester::action_result join(Tester& tester, name account, std::string_view chain_code,
                                    name signer = opreg_account) {
   try {
      tester.push_action(opreg_account, joinchain_action, signer, fc::mutable_variant_object()
         ("account", account)("chain_code", chain_code));
      return Tester::success();
   } catch (const fc::exception& ex) {
      return Tester::error(ex.top_message());
   }
}

/// Push `sysio.opreg::leavechain(account, chain_code)` signed by the operator.
template <typename Tester>
typename Tester::action_result leave(Tester& tester, name account, std::string_view chain_code) {
   try {
      tester.push_action(opreg_account, leavechain_action, account, fc::mutable_variant_object()
         ("account", account)("chain_code", chain_code));
      return Tester::success();
   } catch (const fc::exception& ex) {
      return Tester::error(ex.top_message());
   }
}

/// One outpost a fixture's batch operators serve: its `sysio.chains` code and chain family.
struct outpost {
   std::string_view                   code;
   sysio::opp::types::ChainKind       kind;
};

/// Link each of `accounts` to a fresh key of every distinct kind among `outposts`, then join each to
/// every outpost: the state a bootstrap leaves its batch operators in before `schbatchgps`.
template <typename Tester>
void serve(Tester& tester, const std::vector<name>& accounts, const std::vector<outpost>& outposts) {
   std::vector<sysio::opp::types::ChainKind> kinds;
   for (const outpost& chain : outposts) {
      if (std::find(kinds.begin(), kinds.end(), chain.kind) == kinds.end()) kinds.push_back(chain.kind);
   }
   for (const name account : accounts) {
      for (const sysio::opp::types::ChainKind kind : kinds) link_fresh_key(tester, account, kind);
      for (const outpost& chain : outposts) {
         BOOST_REQUIRE_EQUAL(Tester::success(), join(tester, account, chain.code));
      }
   }
}

/// `account`'s decoded `servchains` row on `chain_code`, or null when it has none. `opreg_abi` holds
/// sysio.opreg's ABI. The row key is (chain, account), the `[scope][id]` layout `get_row_by_id` reads.
template <typename Tester>
fc::variant membership(Tester& tester, abi_serializer& opreg_abi, name account, std::string_view chain_code) {
   const auto data = tester.get_row_by_id(opreg_account, name{fc::slug_name{chain_code}.value}, membership_table,
                                          account.to_uint64_t());
   return data.empty() ? fc::variant()
                       : opreg_abi.binary_to_variant(membership_type, data, Tester::fixture_abi_yield_function());
}

} // namespace served_chains

// ---------------------------------------------------------------------------
//  Consensus epoch state written directly, for fixtures that run no batch operators or no outpost
// ---------------------------------------------------------------------------

namespace epoch_state {

inline constexpr auto epoch_account = "sysio.epoch"_n;
inline constexpr auto state_table   = "epochstate"_n;
inline constexpr auto state_type    = "epoch_state";
inline constexpr auto genesis_table = "netgenesis"_n;
inline constexpr auto genesis_type  = "network_genesis";

/// Field names of an `epochstate` / `netgenesis` row.
namespace field {
inline constexpr auto chain_code             = "chain_code";
inline constexpr auto current_epoch_index    = "current_epoch_index";
inline constexpr auto current_epoch_start    = "current_epoch_start";
inline constexpr auto next_epoch_start       = "next_epoch_start";
inline constexpr auto current_batch_op_group = "current_batch_op_group";
inline constexpr auto batch_op_groups        = "batch_op_groups";
inline constexpr auto is_paused              = "is_paused";
inline constexpr auto previous_group         = "previous_group";
inline constexpr auto genesis_at             = "genesis_at";
} // namespace field

/// `chain`'s `epochstate` row decoded with `epoch_abi`, or the row a chain starts from (index 0, no window).
template <typename Tester>
fc::mutable_variant_object state_of(Tester& tester, const abi_serializer& epoch_abi, std::string_view chain) {
   const uint64_t chain_code = fc::slug_name{chain}.value;
   const auto     data       = tester.get_row_by_id(epoch_account, epoch_account, state_table, chain_code);
   if (!data.empty()) {
      return fc::mutable_variant_object(
         epoch_abi.binary_to_variant(state_type, data, Tester::fixture_abi_yield_function()).get_object());
   }
   return fc::mutable_variant_object()
      (field::chain_code, chain_code)
      (field::current_epoch_index, 0u)
      (field::current_epoch_start, fc::time_point())
      (field::next_epoch_start, fc::time_point())
      (field::current_batch_op_group, 0u)
      (field::batch_op_groups, std::vector<std::vector<name>>{})
      (field::is_paused, false)
      (field::previous_group, std::vector<name>{});
}

/// Store `state` as its chain's `epochstate` row.
template <typename Tester>
void write_state(Tester& tester, const abi_serializer& epoch_abi, const fc::mutable_variant_object& state) {
   write_kv_row(tester, epoch_account, state_table, state[field::chain_code].as_uint64(),
                epoch_abi.variant_to_binary(state_type, state, Tester::fixture_abi_yield_function()));
}

/// Seat `groups` as `chain`'s schedule window, front group on duty, as `schbatchgps` would from that many serving
/// batch operators, for a fixture that registers none. The chain's index and timing are left as they are.
template <typename Tester>
void seat_window(Tester& tester, const abi_serializer& epoch_abi, std::string_view chain,
                 const std::vector<std::vector<name>>& groups) {
   fc::mutable_variant_object state = state_of(tester, epoch_abi, chain);
   state(field::batch_op_groups, groups)(field::current_batch_op_group, 0u);
   write_state(tester, epoch_abi, state);
}

/// Leave the state a network genesis leaves, for a fixture that runs no outpost (a genesis advance needs an active
/// outpost with a seated window): `chain` at its epoch 1 from the head block, `epoch_duration_sec` long, and the
/// `netgenesis` marker.
template <typename Tester>
void start_network(Tester& tester, const abi_serializer& epoch_abi, std::string_view chain,
                   uint32_t epoch_duration_sec) {
   const fc::time_point       now   = tester.control->head().block_time();
   fc::mutable_variant_object state = state_of(tester, epoch_abi, chain);
   state(field::current_epoch_index, 1u)
        (field::current_epoch_start, now)
        (field::next_epoch_start, now + fc::seconds(epoch_duration_sec));
   write_state(tester, epoch_abi, state);
   write_kv_row(tester, epoch_account, genesis_table, genesis_table.to_uint64_t(),
                epoch_abi.variant_to_binary(genesis_type, fc::mutable_variant_object()(field::genesis_at, now),
                                            Tester::fixture_abi_yield_function()));
}

} // namespace epoch_state

/// Mirrors of sysio.roa's `nodeownerreg` audit values (`reg_status` / `reject_reason` in sysio.roa.hpp).
namespace nodeownerreg {
inline constexpr uint64_t status_confirmed            = 0;
inline constexpr uint64_t status_rejected             = 1;
inline constexpr uint64_t reason_name_invalid         = 1;
inline constexpr uint64_t reason_owner_not_account    = 2;
inline constexpr uint64_t reason_account_key_mismatch = 3;
inline constexpr uint64_t reason_duplicate            = 4;
inline constexpr uint64_t reason_link_key_mismatch    = 5;
inline constexpr uint64_t reason_tier_cap_reached     = 6;
inline constexpr uint64_t reason_account_exists       = 7;
} // namespace nodeownerreg

namespace nodeowners {
/// Host-side mirror of the consensus Tier-1 cap in sysio.system/emissions.hpp.
inline constexpr uint32_t tier1_cap = 21;
inline constexpr uint8_t tier1 = 1;
inline constexpr auto account = "sysio.roa"_n;
inline constexpr auto table = "nodeowners"_n;

/// Count authoritative owners in one generation, independently of the optional emissions mirror.
template <typename Tester>
uint32_t count(Tester& tester, abi_serializer& serializer, uint8_t tier, uint64_t generation = 0) {
   const auto scope_start = make_kv_scoped_key(generation, uint64_t{0});
   const auto scope_end = make_kv_scoped_key(generation + 1, uint64_t{0});
   const auto table_id = compute_table_id(table.to_uint64_t());
   uint32_t result = 0;
   auto cursor = tester.control->kv().open_row_cursor();
   kv_row stored;
   for (bool more = cursor->seek_lower({account, table_id, scope_start.to_string_view()}, stored);
        more && stored.code == account && stored.table_id == table_id && stored.key < scope_end.to_string_view();
        more = cursor->next(stored)) {
      const std::vector<char> bytes(stored.value.begin(), stored.value.end());
      const auto row = serializer.binary_to_variant(
         "nodeowners", bytes, Tester::fixture_abi_yield_function());
      if (row["tier"].template as<uint8_t>() == tier) ++result;
   }
   return result;
}

/// Fill Tier 1 to a requested occupancy, including the owner installed by genesis.
template <typename Tester>
void fill_tier1(Tester& tester, abi_serializer& serializer, uint32_t occupancy = tier1_cap) {
   const auto initial = count(tester, serializer, tier1);
   BOOST_REQUIRE_LE(initial, occupancy);
   BOOST_REQUIRE_LE(occupancy, tier1_cap);
   for (uint32_t i = initial; i < occupancy; ++i) {
      const name owner{std::string{"cap"} + char('a' + i)};
      tester.create_accounts({owner}, false, false, false, false);
      BOOST_REQUIRE_EQUAL(Tester::success(), push_contract_action(
         tester, account, serializer, account, "forcereg"_n,
         fc::mutable_variant_object()("owner", owner)("tier", tier1)));
      tester.produce_block();
   }
   BOOST_REQUIRE_EQUAL(count(tester, serializer, tier1), occupancy);
}
} // namespace nodeowners

// ---------------------------------------------------------------------------
//  sysio.andon: the depot's emergency stop, for the suites of the contracts it freezes
// ---------------------------------------------------------------------------

namespace andon {

/// The account the depot deploys sysio.andon on, which every frozen contract reads.
inline constexpr auto account = "sysio.andon"_n;
/// The configuration authority, which may also pull and clear the cord.
inline constexpr auto system_account = "sysio"_n;

/// Create sysio.andon's account when it does not exist, deploy `wasm` and `abi` on it privileged (as the
/// depot deploys it) and load its ABI into `ser`.
template <typename Tester>
void deploy(Tester& tester, abi_serializer& ser, const std::vector<uint8_t>& wasm, const std::vector<char>& abi) {
   if (tester.control->find_account(account) == nullptr) tester.create_accounts({account});
   tester.set_code(account, wasm);
   tester.set_abi(account, abi.data());
   tester.set_privileged(account);
   // Match bootstrap: governance controls the Andon account's active authority.
   tester.set_authority(account, config::active_name,
                        authority{permission_level{system_account, config::active_name}}, config::owner_name);
   tester.produce_blocks();
   load_account_abi(tester, account, ser);
}

/// Pull with Andon active authorization, signed by its delegated actor.
template <typename Tester>
typename Tester::action_result pull(Tester& tester, abi_serializer& ser, name actor = system_account,
                                    const std::string& reason = "test") {
   return push_contract_action_and_produce_block(tester, account, ser, actor, "pull"_n,
                                                 fc::mutable_variant_object()("reason", reason), {{account, config::active_name}});
}

/// Clear with Andon active authorization, signed by its delegated actor.
template <typename Tester>
typename Tester::action_result clear(Tester& tester, abi_serializer& ser, name actor = system_account,
                                     const std::string& note = "test") {
   return push_contract_action_and_produce_block(tester, account, ser, actor, "clear"_n,
                                                 fc::mutable_variant_object()("note", note), {{account, config::active_name}});
}

/// The message of the refusal every frozen signed action raises (`andon::FROZEN_MESSAGE`); a suite
/// compares against `wasm_assert_msg(frozen_message)`.
inline constexpr const char* frozen_message = "the andon cord is pulled: funds cannot leave custody";

} // namespace andon

// ---------------------------------------------------------------------------
//  sysio.system: what the depot runs on sysio, for the suites of contracts that send it actions
// ---------------------------------------------------------------------------

namespace system_contract {

/// Deploy `wasm` and `abi` on sysio in place of the tester's sysio.bios, so the sysio.system actions another
/// contract sends inline (as sysio.bond sends `addsubjex`) reach a contract that defines them. Create system
/// accounts first: under sysio.system a new account gets only the base RAM of `newaccount`.
template <typename Tester>
void deploy(Tester& tester, const std::vector<uint8_t>& wasm, const std::vector<char>& abi) {
   tester.set_code(config::system_account_name, wasm);
   tester.set_abi(config::system_account_name, abi.data());
   tester.produce_blocks();
}

} // namespace system_contract

/// secp256k1 encodings for tests that feed the depot keys which are not curve points. libsecp256k1
/// (through fc) is the oracle the contracts' own `mod_exp` arithmetic is checked against.
namespace secp256k1 {

/// One big-endian field element (an x or y coordinate).
using coordinate = std::array<char, 32>;

/// The field prime p = 2^256 - 2^32 - 977, big-endian.
inline coordinate field_prime() {
   coordinate p;
   p.fill(char(0xff));
   p[27] = char(0xfe);
   p[30] = char(0xfc);
   p[31] = char(0x2f);
   return p;
}

/// x + p: a non-canonical spelling of x, which still fits in 32 bytes for any x < 2^32 + 977.
inline coordinate plus_field_prime(const coordinate& x) {
   const auto p = field_prime();
   coordinate sum;
   unsigned carry = 0;
   for (size_t i = sum.size(); i-- > 0;) {
      carry += static_cast<uint8_t>(x[i]) + static_cast<uint8_t>(p[i]);
      sum[i] = static_cast<char>(carry & 0xffu);
      carry >>= 8;
   }
   BOOST_REQUIRE_EQUAL(carry, 0u);
   return sum;
}

/// A compressed SEC1 encoding from a prefix byte and an x-coordinate.
inline fc::em::public_key_data compressed_key(uint8_t prefix, const coordinate& x) {
   fc::em::public_key_data key{};
   key[0] = static_cast<char>(prefix);
   std::copy(x.begin(), x.end(), key.begin() + 1);
   return key;
}

/// Whether libsecp256k1 parses the bytes as a compressed point.
inline bool is_point(const fc::em::public_key_data& key) {
   try {
      fc::em::public_key::serialize_uncompressed(key);
      return true;
   } catch (const fc::exception&) {
      return false;
   }
}

/// The smallest x >= 1 that has a curve point, or (with `want_point` false) the smallest that has none.
inline coordinate smallest_x(bool want_point) {
   for (uint8_t candidate = 1; candidate != 0; ++candidate) {
      coordinate x{};
      x.back() = static_cast<char>(candidate);
      if (is_point(compressed_key(0x02, x)) == want_point) return x;
   }
   BOOST_FAIL("no small x-coordinate found");
   return {};
}

} // namespace secp256k1

} // namespace sysio_system::test_support
