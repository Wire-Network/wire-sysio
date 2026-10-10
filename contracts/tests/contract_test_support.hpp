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

#include <string>

namespace sysio_system::test_support {

using namespace sysio::chain;

/// Load an account's deployed ABI into a serializer used by contract integration fixtures.
template <typename Tester>
void load_account_abi(Tester& tester, name account, abi_serializer& out_ser) {
   const auto* metadata = tester.control->find_account_metadata(account);
   BOOST_REQUIRE(metadata != nullptr);
   abi_def parsed;
   BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(metadata->abi, parsed), true);
   out_ser.set_abi(std::move(parsed), abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
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
      abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
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
   const auto& index = tester.control->db().template get_index<kv_index, by_code_key>();
   const auto scope_start = make_kv_scoped_key(generation, uint64_t{0});
   const auto scope_end = make_kv_scoped_key(generation + 1, uint64_t{0});
   const auto table_id = compute_table_id(table.to_uint64_t());
   auto it = index.lower_bound(boost::make_tuple(account, table_id, scope_start.to_string_view()));
   const auto end = index.lower_bound(boost::make_tuple(account, table_id, scope_end.to_string_view()));
   uint32_t result = 0;
   for (; it != end; ++it) {
      const std::vector<char> bytes(it->value.begin(), it->value.end());
      const auto row = serializer.binary_to_variant(
         "nodeowners", bytes, abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
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

} // namespace sysio_system::test_support
