#pragma once
#include <ranges>

#include <fc/crypto/crypto_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>

namespace fc::crypto {
template <typename KeyType>
std::string storage_to_base58_str(const KeyType& key, const char* prefix_chars = nullptr,
                                  const fc::yield_function_t& yield = {}) {
   using data_type = KeyType::data_type;
   checksum_data<data_type> wrapper;
   wrapper.data = key.serialize();
   yield();
   wrapper.check = checksum_data<data_type>::calculate_checksum(wrapper.data, prefix_chars);
   yield();
   auto packed = raw::pack(wrapper);
   yield();
   auto data_str = to_base58(packed.data(), packed.size(), yield);
   yield();
   if (prefix_chars) {
      data_str = std::string(prefix_chars) + "_" + data_str;
   }
   yield();

   return data_str;
}

template <typename Storage, const char* const * Prefixes, int DefaultPosition = -1>
struct to_native_string_from_public_key_visitor : public fc::visitor<std::string> {
   explicit to_native_string_from_public_key_visitor(const fc::yield_function_t& yield)
      : _yield(yield) {};

   std::string operator()(const em::public_key_shim& em_pub_key) const {
      auto em_bytes = em_pub_key.unwrapped().serialize_uncompressed() | std::ranges::to<std::vector<std::uint8_t>>();
      return to_hex(em_bytes, true);
   }

   std::string operator()(const bls::public_key_shim& bls_pub_key_shim) const {
      return bls_pub_key_shim.unwrapped().to_string();
   }

   std::string operator()(const ed::public_key_shim& ed_pub_key_shim) const {
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED keys are not implemented yet");
   }

   template <typename KeyType>
   std::string operator()(const KeyType& key) const {
      constexpr int position = fc::get_index<Storage, KeyType>();
      constexpr bool is_default = position == DefaultPosition;
      constexpr const char* prefix_chars = !is_default ? Prefixes[position] : nullptr;
      auto data_str = storage_to_base58_str(key, prefix_chars, _yield);
      if (position == 0) {
         return std::string(fc::crypto::constants::public_key_legacy_prefix) + data_str;
      } else {
         return std::string(fc::crypto::constants::public_key_base_prefix) + "_" + data_str;
      }
   }

   const fc::yield_function_t _yield;
};

/**
 * @brief Private Key to Native Chain String Serializer
 *
 * Create a string representation of this private key,
 * which is in the format that the
 * `chain_kind_t` -> `chain_key_type_t` expects.
 *
 * @tparam Storage private_key::storage_type
 * @tparam Prefixes private key prefixes array
 * @tparam DefaultPosition default storage position
 */
template <typename Storage, const char* const * Prefixes, int DefaultPosition = -1>
struct to_native_string_from_private_key_visitor : public fc::visitor<std::string> {
   explicit to_native_string_from_private_key_visitor(const fc::yield_function_t& yield)
      : _yield(yield) {};

   std::string operator()(const em::private_key_shim& em_priv_key) const {
      return to_hex(em_priv_key.serialize(), true);
   }

   std::string operator()(const bls::private_key_shim& bls_priv_key_shim) const {
      return bls_priv_key_shim.to_string();
   }

   std::string operator()(const ed::private_key_shim& ed_priv_key_shim) const {
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED keys are not implemented yet");
   }

   template <typename KeyType>
   std::string operator()(const KeyType& key) const {
      constexpr int position = fc::get_index<Storage, KeyType>();
      constexpr bool is_default = position == DefaultPosition;
      constexpr const char* prefix_chars = !is_default ? Prefixes[position] : nullptr;
      return storage_to_base58_str(key, prefix_chars, _yield);
   }

   const fc::yield_function_t _yield;
};

/**
 * @brief Signature to Native Chain String Serializer
 *
 * Create a string representation of this signature,
 * which is in the format that the
 * `chain_kind_t` -> `chain_key_type_t` expects.
 *
 * @tparam Storage signature::storage_type
 * @tparam Prefixes signature prefixes array
 * @tparam DefaultPosition default storage position
 */
template <typename Storage, const char* const * Prefixes, int DefaultPosition = -1>
struct to_native_string_from_signature_visitor : public fc::visitor<std::string> {
   explicit to_native_string_from_signature_visitor(const fc::yield_function_t& yield)
      : _yield(yield) {};

   std::string operator()(const em::signature_shim& em_sig) const {
      auto em_bytes = em_sig.serialize() | std::ranges::to<std::vector<std::uint8_t>>();
      return to_hex(em_bytes, true);
   }

   std::string operator()(const bls::signature_shim& bls_sig_shim) const {
      return bls_sig_shim.to_string();
   }

   std::string operator()(const ed::signature_shim& ed_sig_shim) const {
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED keys are not implemented yet");
   }

   template <typename SigType>
   std::string operator()(const SigType& key) const {
      constexpr int position = fc::get_index<Storage, SigType>();
      constexpr bool is_default = position == DefaultPosition;
      constexpr const char* prefix_chars = !is_default ? Prefixes[position] : nullptr;
      return storage_to_base58_str(key, prefix_chars, _yield);
   }

   const fc::yield_function_t _yield;
};

template <fc::crypto::chain_key_type_t ChainKeyType>
public_key::storage_type from_native_string_to_public_key_shim(const std::string& public_key_str) {
   if (public_key_str.empty()) {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "Public key string cannot be empty");
   }
   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_wire_bls)
      return bls::public_key_shim(bls::public_key(public_key_str).serialize());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_ethereum)
      return em::public_key_shim(em::public_key::from_native_string(public_key_str).serialize());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_solana)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED support is not yet implemented");

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_sui)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "SUI support is not yet implemented");

   return fc::crypto::public_key::parse_base58(public_key_str);
}

template <fc::crypto::chain_key_type_t ChainKeyType>
public_key from_native_string_to_public_key(const std::string& private_key_str) {
   return public_key{from_native_string_to_public_key_shim<ChainKeyType>(private_key_str)};
}

template <fc::crypto::chain_key_type_t ChainKeyType>
private_key::storage_type from_native_string_to_private_key_shim(const std::string& private_key_str) {
   if (private_key_str.empty()) {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "Private key string cannot be empty");
   }
   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_wire_bls)
      return bls::private_key_shim(bls::private_key(private_key_str).get_secret());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_ethereum)
      return em::private_key_shim(em::private_key::from_native_string(private_key_str).get_secret());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_solana)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED support is not yet implemented");

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_sui)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "SUI support is not yet implemented");

   return fc::crypto::private_key::priv_parse_base58(private_key_str);
}

template <fc::crypto::chain_key_type_t ChainKeyType>
private_key from_native_string_to_private_key(const std::string& private_key_str) {
   return private_key{from_native_string_to_private_key_shim<ChainKeyType>(private_key_str)};
}

template <fc::crypto::chain_key_type_t ChainKeyType>
signature::storage_type from_native_string_to_signature_shim(const std::string& signature_str) {
   if (signature_str.empty()) {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "Private key string cannot be empty");
   }
   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_wire_bls)
      return bls::signature_shim(bls::signature(signature_str).serialize());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_ethereum)
      return em::signature_shim(ethereum::to_em_signature(signature_str));

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_solana)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED support is not yet implemented");

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_sui)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "SUI support is not yet implemented");

   return fc::crypto::signature::sig_parse_base58(signature_str);
}

template <fc::crypto::chain_key_type_t ChainKeyType>
signature from_native_string_to_signature(const std::string& private_key_str) {
   return signature{from_native_string_to_signature_shim<ChainKeyType>(private_key_str)};
}


template <typename, const char* const *, int, typename...>
struct base58_str_parser_impl;

template <typename Result, const char* const * Prefixes, int Position, typename KeyType, typename... Rem>
struct base58_str_parser_impl<Result, Prefixes, Position, KeyType, Rem...> {
   static Result apply(const std::string& prefix_str, const std::string& data_str) {
      using data_type = KeyType::data_type;
      using wrapper = checksum_data<data_type>;
      constexpr auto prefix = Prefixes[Position];

      if (prefix == prefix_str) {
         auto bin = fc::from_base58(data_str);
         fc::datastream<const char*> unpacker(bin.data(), bin.size());
         wrapper wrapped;
         fc::raw::unpack(unpacker, wrapped);
         FC_ASSERT(!unpacker.remaining(), "decoded base58 length too long");
         auto checksum = wrapper::calculate_checksum(wrapped.data, prefix);
         FC_ASSERT(checksum == wrapped.check);
         return Result(KeyType(wrapped.data));
      }

      return base58_str_parser_impl<Result, Prefixes, Position + 1, Rem...>::apply(prefix_str, data_str);
   }
};

template <typename Result, const char* const * Prefixes, int Position>
struct base58_str_parser_impl<Result, Prefixes, Position> {
   static Result apply(const std::string& prefix_str, const std::string& data_str) {
      FC_ASSERT(false, "No matching suite type for ${prefix}_${data}", ("prefix", prefix_str)("data",data_str));
   }
};

template <typename, const char* const * Prefixes>
struct base58_str_parser;

/**
 * Destructure a variant and call the parse_base58str on it
 * @tparam Ts
 * @param base58str
 * @return
 */
template <const char* const * Prefixes, typename... Ts>
struct base58_str_parser<std::variant<Ts...>, Prefixes> {
   static std::variant<Ts...> apply(const std::string& base58str) {
      const auto pivot = base58str.find('_');
      FC_ASSERT(pivot != std::string::npos, "No delimiter in data, cannot determine suite type: ${str}",
                ("str", base58str));

      const auto prefix_str = base58str.substr(0, pivot);
      auto data_str = base58str.substr(pivot + 1);
      FC_ASSERT(!data_str.empty(), "Data only has suite type prefix: ${str}", ("str", base58str));

      // ** Original version **
      // return base58_str_parser_impl<fc::static_variant<Ts...>, Prefixes, 0, Ts...>::apply(prefix_str, data_str);
      // ** NEW: added ternary for EM / K1
      // if(prefix_str == std::string("EM")) {
      //    return base58_str_parser_impl<fc::static_variant<Ts...>, Prefixes, 3, Ts...>::apply(prefix_str, data_str);
      // } else {
      // }

      return base58_str_parser_impl<std::variant<Ts...>, Prefixes, 0, Ts...>::apply(prefix_str, data_str);
   }
};


template <typename Storage, const char* const * Prefixes, int DefaultPosition = -1>
struct base58str_visitor : public fc::visitor<std::string> {
   explicit base58str_visitor(const fc::yield_function_t& yield)
      : _yield(yield) {};

   template <typename KeyType>
   std::string operator()(const KeyType& key) const {
      constexpr int position = fc::get_index<Storage, KeyType>();
      constexpr bool is_default = position == DefaultPosition;
      constexpr const char* prefix_chars = !is_default ? Prefixes[position] : nullptr;
      return storage_to_base58_str(key, prefix_chars, _yield);
   }

   const fc::yield_function_t _yield;
};


}