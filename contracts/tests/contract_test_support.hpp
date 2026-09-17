#pragma once

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

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

/// Build and sign one ABI-encoded contract-action transaction.
template <typename Tester>
signed_transaction create_contract_action_transaction(Tester& tester, name contract, abi_serializer& serializer,
                                                       name signer, name action_name,
                                                       const fc::variant_object& data) {
   action act;
   act.account = contract;
   act.name = action_name;
   act.data = serializer.variant_to_binary(
      serializer.get_action_type(action_name), data,
      abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
   act.authorization = std::vector<permission_level>{{signer, config::active_name}};

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
                                                 const fc::variant_object& data) {
   auto trx = create_contract_action_transaction(tester, contract, serializer, signer, action_name, data);
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

/// Push one ABI-encoded action and land it in its own block for stable TaPoS.
template <typename Tester>
typename Tester::action_result push_contract_action_and_produce_block(
   Tester& tester, name contract, abi_serializer& serializer, name signer, name action_name,
   const fc::variant_object& data) {
   try {
      push_contract_action_trace(tester, contract, serializer, signer, action_name, data);
      tester.produce_block();
      return Tester::success();
   } catch (const fc::exception& ex) {
      return Tester::error(ex.top_message());
   }
}

// ---------------------------------------------------------------------------
//  sysio.chains::outpost_addrs builders
//
//  `regchain` and `setoutpost` both take the remote outpost contract identities
//  as one nested struct. These build its variant form so a test states only the
//  addresses it cares about; the contract validates the set against the row's
//  ChainKind (see validate_outpost_addrs in sysio.chains.cpp).
// ---------------------------------------------------------------------------

/// Every field empty — a chain registered before its remote contracts exist.
/// Valid for any kind; both operator daemons fail closed and skip such a row.
inline fc::mutable_variant_object no_outpost_mvo() {
   return fc::mutable_variant_object()
      ("opp_addr",               std::string{})
      ("opp_inbound_addr",       std::string{})
      ("operator_registry_addr", std::string{})
      ("source_deposit_addr",    std::string{});
}

/// EVM form: each role is a distinct 0x-prefixed 20-byte hex contract address.
inline fc::mutable_variant_object evm_outpost_mvo(std::string_view opp,
                                                  std::string_view opp_inbound,
                                                  std::string_view operator_registry,
                                                  std::string_view source_deposit) {
   return fc::mutable_variant_object()
      ("opp_addr",               std::string{opp})
      ("opp_inbound_addr",       std::string{opp_inbound})
      ("operator_registry_addr", std::string{operator_registry})
      ("source_deposit_addr",    std::string{source_deposit});
}

/// SVM form: one base58 program id serves every role, so the other three fields
/// must stay empty — the contract rejects a set that fills them in.
inline fc::mutable_variant_object svm_outpost_mvo(std::string_view program_id) {
   return fc::mutable_variant_object()
      ("opp_addr",               std::string{program_id})
      ("opp_inbound_addr",       std::string{})
      ("operator_registry_addr", std::string{})
      ("source_deposit_addr",    std::string{});
}

} // namespace sysio_system::test_support
