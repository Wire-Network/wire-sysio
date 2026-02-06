#pragma once

#include <fc/crypto/crypto_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/bls_common.hpp>

#include <ranges>
#include <string>
#include <string_view>
#include <tuple>

namespace fc::crypto {

public_key::storage_type parse_unknown_wire_public_key_str(const std::string& str);
private_key::storage_type parse_unknown_wire_private_key_str(const std::string& str);
signature::storage_type parse_unknown_wire_signature_str(const std::string& str);

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
      return ed_priv_key_shim.to_string(_yield);
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
      return em_sig.to_string();
   }

   std::string operator()(const bls::signature_shim& bls_sig_shim) const {
      return bls_sig_shim.to_string();
   }

   std::string operator()(const ed::signature_shim& ed_sig_shim) const {
      return ed_sig_shim.to_string(_yield);
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

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_ethereum)
      return em::public_key_shim(em::public_key::from_string(public_key_str).serialize());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_solana)
      return ed::public_key_shim::from_base58_string(public_key_str);

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_sui)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "SUI support is not yet implemented");

   return parse_unknown_wire_public_key_str(public_key_str);
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

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_ethereum)
      return em::private_key_shim(em::private_key::from_native_string(private_key_str).get_secret());

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_solana)
      return ed::private_key_shim::from_base58_string(private_key_str);

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_sui)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "SUI support is not yet implemented");

   return parse_unknown_wire_private_key_str(private_key_str);
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

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_ethereum)
      return em::signature_shim(ethereum::to_em_signature(signature_str));

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_solana)
      return ed::signature_shim::from_base58_string(signature_str);

   if constexpr (ChainKeyType == fc::crypto::chain_key_type_t::chain_key_type_sui)
      FC_THROW_EXCEPTION(fc::unsupported_exception, "SUI support is not yet implemented");

   return parse_unknown_wire_signature_str(signature_str);
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
      FC_ASSERT(false, "No matching suite type for {}_{}", prefix_str, data_str);
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
      FC_ASSERT(pivot != std::string::npos, "No delimiter in data, cannot determine suite type: {}", base58str);

      const auto prefix_str = base58str.substr(0, pivot);
      auto data_str = base58str.substr(pivot + 1);
      FC_ASSERT(!data_str.empty(), "Data only has suite type prefix: {}", base58str);

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

template<typename Data>
std::string to_wif( const Data& secret, const fc::yield_function_t& yield ) {
   const size_t size_of_data_to_hash = sizeof(typename Data::data_type) + 1;
   const size_t size_of_hash_bytes = 4;
   char data[size_of_data_to_hash + size_of_hash_bytes];
   data[0] = (char)0x80; // this is the Bitcoin MainNet code
   memcpy(&data[1], (const char*)&secret.serialize(), sizeof(typename Data::data_type));
   sha256 digest = sha256::hash(data, size_of_data_to_hash);
   digest = sha256::hash(digest);
   memcpy(data + size_of_data_to_hash, (char*)&digest, size_of_hash_bytes);
   return to_base58(data, sizeof(data), yield);
}

template<typename Data>
Data from_wif( const std::string& wif_key ) {
   auto wif_bytes = from_base58(wif_key);
   FC_ASSERT(wif_bytes.size() >= 5);
   auto key_bytes = std::vector<char>(wif_bytes.begin() + 1, wif_bytes.end() - 4);
   fc::sha256 check = fc::sha256::hash(wif_bytes.data(), wif_bytes.size() - 4);
   fc::sha256 check2 = fc::sha256::hash(check);

   FC_ASSERT(memcmp( (char*)&check, wif_bytes.data() + wif_bytes.size() - 4, 4 ) == 0 ||
             memcmp( (char*)&check2, wif_bytes.data() + wif_bytes.size() - 4, 4 ) == 0 );
   FC_ASSERT(key_bytes.size() == sizeof(typename Data::data_type), "Invalid key size for type {}", typeid(Data).name());

   Data d{};
   memcpy(d._data.data(), key_bytes.data(), key_bytes.size());
   return d;
}

// Given PUB_K1_xxx returns <'PUB','K1','xxx'>
inline std::tuple<std::string, std::string, std::string> parse_base_prefixes(std::string_view str) {
   const auto first = str.find('_');
   if (first == std::string::npos)
      return {};

   const auto second = str.find('_', first + 1);
   if (second == std::string::npos)
      return {};

   // Ensure there is data after the second one
   if (second + 1 >= str.size())
      return {};

   return std::tuple<std::string, std::string, std::string>{
      str.substr(0, first), str.substr(first + 1, second - first - 1), str.substr(second + 1)};
}

inline private_key::storage_type parse_unknown_wire_private_key_str(const std::string& str) {
   FC_ASSERT(!str.empty(), "Empty private key string");
   const auto pivot = str.find('_');
   if (pivot == std::string::npos) {
      // wif import
      using default_type = std::variant_alternative_t<0, private_key::storage_type>;
      return private_key::storage_type(from_wif<default_type>(str));
   }

   constexpr auto prefix = fc::crypto::constants::private_key_base_prefix;
   auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
   FC_ASSERT(!base_prefix.empty(), "Invalid private key prefixes: {}", str.substr(0, 10) + "...");
   FC_ASSERT(prefix == base_prefix, "Private Key has invalid prefix: {}", base_prefix);
   if (type_prefix == private_key::key_prefix(private_key::key_type::k1) ||
      type_prefix == private_key::key_prefix(private_key::key_type::r1)) {
      return base58_str_parser<private_key::storage_type, fc::crypto::constants::private_key_prefix>::apply(type_prefix + '_' + data_str);
   }
   if (type_prefix == private_key::key_prefix(private_key::key_type::bls)) {
      return bls::private_key_shim(bls::private_key(str).get_secret());
   }
   FC_ASSERT(false, "Unknown private key suite: {} in private_key", type_prefix);
}

inline bool is_legacy_public_key_str(const std::string& str) {
   constexpr auto legacy_prefix = fc::crypto::constants::public_key_legacy_prefix;
   return prefix_matches(legacy_prefix, str) && str.find('_') == std::string::npos;
}

inline public_key::storage_type parse_unknown_wire_public_key_str(const std::string& str) {
   if(is_legacy_public_key_str(str)) {
      auto sub_str = str.substr(const_strlen(fc::crypto::constants::public_key_legacy_prefix));
      using default_type = typename std::variant_alternative_t<0, public_key::storage_type>; //public_key::storage_type::template type_at<0>;
      using data_type = default_type::data_type;
      using wrapper = checksum_data<data_type>;
      auto bin = fc::from_base58(sub_str);
      FC_ASSERT(bin.size() == sizeof(data_type) + sizeof(uint32_t), "");
      auto wrapped = fc::raw::unpack<wrapper>(bin);
      FC_ASSERT(wrapper::calculate_checksum(wrapped.data) == wrapped.check);
      return public_key::storage_type(default_type(wrapped.data));
   }

   constexpr auto prefix = fc::crypto::constants::public_key_base_prefix;
   auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
   FC_ASSERT(!base_prefix.empty(), "Invalid public key prefixes: {}", str);
   FC_ASSERT(prefix == base_prefix, "Public Key has invalid prefix: {}", str);
   if (type_prefix == public_key::key_prefix(public_key::key_type::k1) ||
      type_prefix == public_key::key_prefix(public_key::key_type::r1) ||
      type_prefix == public_key::key_prefix(public_key::key_type::wa)) {
      return base58_str_parser<public_key::storage_type, fc::crypto::constants::public_key_prefix>::apply(type_prefix + '_' + data_str);
   }
   if (type_prefix == public_key::key_prefix(public_key::key_type::bls)) {
      return bls::deserialize_bls_base64url(str);
   }
   FC_ASSERT(false, "Unknown public key suite: {} in {}", type_prefix, str);
}

inline signature::storage_type parse_unknown_wire_signature_str(const std::string& str) {
   try {
      constexpr auto prefix = fc::crypto::constants::signature_base_prefix;
      auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
      FC_ASSERT(!base_prefix.empty(), "Invalid signature prefixes: {}", str);
      FC_ASSERT(prefix == base_prefix, "Signature has invalid prefix: {}", str);

      if (type_prefix == signature::sig_prefix(signature::sig_type::k1) ||
         type_prefix == signature::sig_prefix(signature::sig_type::r1) ||
         type_prefix == signature::sig_prefix(signature::sig_type::wa)) {
         return base58_str_parser<signature::storage_type, fc::crypto::constants::signature_prefix>::apply(type_prefix + '_' + data_str);
      }
      if (type_prefix == signature::sig_prefix(signature::sig_type::bls)) {
         return bls::sig_parse_base64url(str);
      }
      FC_ASSERT(false, "Unknown signture suite: {} in {}", type_prefix, str);
   } FC_RETHROW_EXCEPTIONS(warn, "error parsing signature {}", str )
}

}