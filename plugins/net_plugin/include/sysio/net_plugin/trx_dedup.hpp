#pragma once

#include <sysio/chain/transaction.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/io/varint.hpp>
#include <fc/time.hpp>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <variant>

namespace sysio {

// Wire size of each fc::crypto::signature variant alternative's data payload.
// -1 = variable or non-trivial serialization, requires fallback to full unpack.
inline constexpr int16_t sig_data_sizes[] = {
   static_cast<int16_t>(sizeof(std::variant_alternative_t<0, fc::crypto::signature::storage_type>::data_type)),  // k1: 65
   static_cast<int16_t>(sizeof(std::variant_alternative_t<1, fc::crypto::signature::storage_type>::data_type)),  // r1: 65
   -1,                                                                                                           // wa: variable
   static_cast<int16_t>(sizeof(std::variant_alternative_t<3, fc::crypto::signature::storage_type>::data_type)),  // em: 65
   static_cast<int16_t>(sizeof(std::variant_alternative_t<4, fc::crypto::signature::storage_type>::data_type)),  // ed: 64
   -1,                                                                                                           // bls: shared_ptr
};
inline constexpr size_t num_sig_types = std::size(sig_data_sizes);
static_assert(std::variant_size_v<fc::crypto::signature::storage_type> == num_sig_types,
              "Signature type count changed, update sig_data_sizes in trx_dedup.hpp");
static_assert(offsetof(chain::transaction_header, expiration) == 0,
              "expiration must be the first field of transaction_header for early dedup memcpy");

/// Parse packed_transaction wire bytes to extract transaction ID and expiration
/// without allocating any heap memory.
///
/// @tparam DS  A datastream type supporting read(char*, size_t) and skip(size_t),
///             compatible with fc::raw::unpack (e.g. mb_peek_datastream or fc::datastream<const char*>).
/// @param ds   Datastream positioned at the start of a net_message (before the which varint).
/// @return     {transaction_id, expiration} on success, or std::nullopt if the message
///             uses variable-size signatures (webauthn/bls) or non-none compression.
template <typename DS>
std::optional<std::pair<chain::transaction_id_type, fc::time_point_sec>>
parse_trx_dedup_info(DS& ds) {
   try {
      // Skip message type varint (net_message variant index)
      fc::unsigned_int which{};
      fc::raw::unpack(ds, which);

      // --- signatures: vector<signature> ---
      // Wire format: unsigned_int(count) + for each: unsigned_int(variant_index) + data_bytes
      fc::unsigned_int sig_count{};
      fc::raw::unpack(ds, sig_count);
      for (uint32_t i = 0; i < sig_count.value; ++i) {
         fc::unsigned_int type_idx{};
         fc::raw::unpack(ds, type_idx);
         if (type_idx.value >= num_sig_types || sig_data_sizes[type_idx.value] < 0)
            return std::nullopt; // unknown or variable-size sig type
         ds.skip(sig_data_sizes[type_idx.value]);
      }

      // --- compression: fc::enum_type<uint8_t, compression_type> ---
      // Serialized as raw uint8_t (1 byte), NOT varint
      uint8_t compression = 0;
      fc::raw::unpack(ds, compression);
      if (compression != static_cast<uint8_t>(chain::packed_transaction::compression_type::none))
         return std::nullopt;

      // --- packed_context_free_data: bytes (vector<char>) ---
      // Wire format: unsigned_int(size) + raw bytes
      fc::unsigned_int cfd_size{};
      fc::raw::unpack(ds, cfd_size);
      ds.skip(cfd_size.value);

      // --- packed_trx: bytes (vector<char>) ---
      // Wire format: unsigned_int(size) + raw bytes
      // For compression_type::none, sha256(packed_trx bytes) == transaction::id()
      fc::unsigned_int packed_trx_size{};
      fc::raw::unpack(ds, packed_trx_size);
      if (packed_trx_size.value < sizeof(fc::time_point_sec))
         return std::nullopt; // too small for a valid transaction header

      // Hash packed_trx bytes and extract expiration in one pass
      fc::sha256::encoder enc;
      fc::time_point_sec expiration;
      char buf[512];
      uint32_t remaining = packed_trx_size.value;
      bool first = true;
      while (remaining > 0) {
         uint32_t n = std::min(remaining, static_cast<uint32_t>(sizeof(buf)));
         ds.read(buf, n);
         enc.write(buf, n);
         if (first) {
            std::memcpy(&expiration, buf, sizeof(fc::time_point_sec));
            first = false;
         }
         remaining -= n;
      }

      return std::optional{std::make_pair(enc.result(), expiration)};
   } catch (const std::exception& e) {
      wlog("early dedup parse failed: {}", e.what());
      return std::nullopt;
   }
}

} // namespace sysio
