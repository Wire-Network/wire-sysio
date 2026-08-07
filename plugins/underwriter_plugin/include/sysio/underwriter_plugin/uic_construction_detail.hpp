#pragma once

#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sysio::underwriter_detail {

/**
 * Return the canonical external caller-address size for a UIC outpost kind.
 * @param kind Outpost chain family carried by the signed UIC.
 * @return 20 for EVM, 32 for SVM, or zero for an unsupported kind.
 */
inline size_t expected_uic_caller_address_size(
   sysio::opp::types::ChainKind kind) {
   switch (kind) {
      case sysio::opp::types::CHAIN_KIND_EVM:
         return fc::crypto::chain_address_size_ethereum;
      case sysio::opp::types::CHAIN_KIND_SVM:
         return fc::crypto::chain_address_size_solana;
      default:
         return 0;
   }
}

/**
 * Populate the non-default chain kind and exact authenticated caller bytes
 * required by every production underwriter UIC.
 *
 * @param uic UIC to populate after all validation succeeds.
 * @param kind Configured outpost transaction signer's chain family.
 * @param address Authenticated chain-native caller bytes.
 * @return true when the kind is supported and the address has its exact
 *         chain-native size; false without mutating `uic` otherwise.
 */
inline bool set_uic_authenticated_caller(
   sysio::opp::attestations::UnderwriteIntentCommit& uic,
   sysio::opp::types::ChainKind kind,
   std::span<const uint8_t> address) {
   const auto expected_size = expected_uic_caller_address_size(kind);
   if (expected_size == 0 || address.size() != expected_size) return false;

   auto* ext_address = uic.mutable_uw_ext_chain_addr();
   ext_address->set_kind(kind);
   ext_address->set_address(address.data(), address.size());
   return true;
}

} // namespace sysio::underwriter_detail
