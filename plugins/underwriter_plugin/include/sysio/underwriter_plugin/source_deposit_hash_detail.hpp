#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include <fc/crypto/keccak256.hpp>

namespace sysio::underwriter {

/// Number of unsigned 64-bit fields in the outpost correlation-hash payload.
inline constexpr size_t SOURCE_DEPOSIT_U64_FIELD_COUNT = 7;

/// Immutable source-outpost fields covered by the `SwapDeposit` correlation
/// hash. The outposts hash the user's accepted `target_amount`, not the depot's
/// mutable AMM quote (`uwreq.dst_amount`). Keeping the two names distinct here
/// prevents a later re-price from invalidating otherwise genuine deposits.
struct source_deposit_hash_input {
   std::span<const char> depositor;
   uint64_t source_amount;
   uint64_t source_token_code;
   uint64_t source_reserve_code;
   uint64_t target_chain_code;
   uint64_t target_token_code;
   uint64_t target_reserve_code;
   uint64_t target_amount;
   uint32_t target_tolerance_bps;
};

/// Reproduce the outposts' packed `SwapDeposit` correlation hash.
inline fc::crypto::keccak256 source_deposit_hash(const source_deposit_hash_input& input) {
   std::vector<uint8_t> packed;
   packed.reserve(input.depositor.size() + SOURCE_DEPOSIT_U64_FIELD_COUNT * sizeof(uint64_t) +
                  sizeof(uint32_t));
   packed.insert(packed.end(), input.depositor.begin(), input.depositor.end());

   const auto append_big_endian = [&](auto value) {
      using value_type = decltype(value);
      static_assert(std::is_unsigned_v<value_type>);
      for (size_t remaining_bytes = sizeof(value_type); remaining_bytes > 0; --remaining_bytes) {
         const size_t shift = (remaining_bytes - 1) * CHAR_BIT;
         packed.push_back(static_cast<uint8_t>(value >> shift));
      }
   };

   append_big_endian(input.source_amount);
   append_big_endian(input.source_token_code);
   append_big_endian(input.source_reserve_code);
   append_big_endian(input.target_chain_code);
   append_big_endian(input.target_token_code);
   append_big_endian(input.target_reserve_code);
   append_big_endian(input.target_amount);
   append_big_endian(input.target_tolerance_bps);

   return fc::crypto::keccak256::hash(
      std::span<const uint8_t>{packed.data(), packed.size()});
}

} // namespace sysio::underwriter
