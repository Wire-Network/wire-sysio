#pragma once

#include <cstring>               // for memset
#include <ostream>               // for std::ostream
#include <sodium.h>              // for ED25519 methods
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/sha512.hpp>  // for generate_shared_secret return type
#include <fc/crypto/base58.hpp>
#include <fc/io/raw.hpp>         // for fc::raw::pack/unpack
#include <fc/io/datastream.hpp>  // for fc::datastream
#include <fc/io/cfile.hpp> // for fc::cfile_datastream
#include <fc/network/message_buffer.hpp>
 
namespace fc { namespace crypto { namespace ed {

// Forward declarations
struct public_key_shim;
struct signature_shim;

/**
 * ED25519 public key (32 bytes)
 */
struct public_key_shim {
   static constexpr size_t size = crypto_sign_PUBLICKEYBYTES;
   using data_type = std::array<unsigned char, size>;
   data_type _data{};

   public_key_shim() = default;
   explicit public_key_shim(const data_type& d): _data(d) {}

   bool valid() const {
      for(auto b : _data) if(b) return true;
      return false;
   }

   data_type serialize() const { return _data; }

   std::string to_string(const fc::yield_function_t& yield)const {
      static_assert(std::same_as<decltype(_data)::value_type, unsigned char>, "Evaluate reinterpret cast if type changes");
      return to_base58(reinterpret_cast<const char*>(_data.data()), _data.size(), yield);
   }

   static public_key_shim from_base58_string(const std::string& str) {
      constexpr size_t max_key_len = 44;
      FC_ASSERT( str.size() <= max_key_len, "Invalid ED25519 public key string length ${s}", ("s", str.size()));
      auto bytes = from_base58(str);
      FC_ASSERT(bytes.size() == size, "Invalid ED25519 public key bytes length ${s}", ("s", bytes.size()));
      public_key_shim result;
      memcpy(result._data.data(), bytes.data(), bytes.size());
      return result;
   }
};

/**
 * ED25519 signature (64 bytes)
 */
struct signature_shim {
   static constexpr size_t size = crypto_sign_BYTES;
   static constexpr bool is_recoverable = false;

   using data_type = std::array<unsigned char, size>;
   data_type _data{};

   signature_shim() = default;
   explicit signature_shim(const data_type& d): _data(d) {}

   data_type serialize() const { return _data; }

   size_t get_hash() const {
      size_t result;
      std::memcpy(&result, _data.data(), sizeof(size_t));
      return result;
   }

   using public_key_type = public_key_shim;
   public_key_shim recover(const sha256&, bool) const {
      FC_THROW_EXCEPTION(exception, "ED25519 signature recovery not supported");
   }

   bool verify(const sha256& digest, const public_key_shim& pub) const;

   std::string to_string(const fc::yield_function_t& yield)const {
      static_assert(std::same_as<decltype(_data)::value_type, unsigned char>, "Evaluate reinterpret cast if type changes");
      return to_base58(reinterpret_cast<const char*>(_data.data()), _data.size(), yield);
   }

   static signature_shim from_base58_string(const std::string& str) {
      constexpr size_t max_sig_len = 88;
      FC_ASSERT( str.size() <= max_sig_len, "Invalid ED25519 signature string length ${s}", ("s", str.size()));
      auto bytes = from_base58(str);
      FC_ASSERT(bytes.size() == size, "Invalid ED25519 signature bytes length ${s}", ("s", bytes.size()));
      signature_shim result;
      memcpy(result._data.data(), bytes.data(), bytes.size());
      return result;
   }
};

/**
 * ED25519 private key (64 bytes secret key from libsodium)
 */
struct private_key_shim {
   static constexpr size_t size = crypto_sign_SECRETKEYBYTES;
   using data_type = std::array<unsigned char, size>;
   data_type _data{};

   private_key_shim() = default;
   explicit private_key_shim(const data_type& d): _data(d) {}

   using public_key_type = public_key_shim;

   public_key_shim get_public_key() const;
   
   signature_shim  sign(const sha256& digest, bool require_canonical) const;
   sha512          generate_shared_secret(const public_key_shim&) const;

   data_type serialize() const { return _data; }

   std::string to_string(const fc::yield_function_t& yield)const {
      static_assert(std::same_as<decltype(_data)::value_type, unsigned char>, "Evaluate reinterpret cast if type changes");
      return to_base58(reinterpret_cast<const char*>(_data.data()), _data.size(), yield);
   }

   static private_key_shim from_base58_string(const std::string& str) {
      constexpr size_t max_key_len = 88;
      FC_ASSERT( str.size() <= max_key_len, "Invalid ED25519 private key string length ${s}", ("s", str.size()));
      auto bytes = from_base58(str);
      FC_ASSERT(bytes.size() == size, "Invalid ED25519 private key bytes length ${s}", ("s", bytes.size()));
      private_key_shim result;
      memcpy(result._data.data(), bytes.data(), bytes.size());
      return result;
   }
};

}}} // namespace fc::crypto::ed

namespace fc { namespace raw {

// raw::pack/unpack for ED public key shim
template<typename Stream>
inline void pack(Stream& s, const crypto::ed::public_key_shim& pk) {
   pack(s, pk.serialize());
}

template<typename Stream>
inline void unpack(Stream& s, crypto::ed::public_key_shim& pk) {
   using data_type = crypto::ed::public_key_shim::data_type;
   data_type buf;
   unpack(s, buf);
   pk = crypto::ed::public_key_shim(buf);
}

// raw::pack/unpack for ED signature shim
template<typename Stream>
inline void pack(Stream& s, const crypto::ed::signature_shim& sig) {
   pack(s, sig.serialize());
}

template<typename Stream>
inline void unpack(Stream& s, crypto::ed::signature_shim& sig) {
   using data_type = crypto::ed::signature_shim::data_type;
   data_type buf;
   unpack(s, buf);
   sig = crypto::ed::signature_shim(buf);
}

// raw::pack/unpack for ED private key shim
template<typename Stream>
inline void pack( Stream& s, const crypto::ed::private_key_shim& sk ) {
   pack(s, sk.serialize());
}

template<typename Stream>
inline void unpack( Stream& s, crypto::ed::private_key_shim& sk ) {
   using data_type = crypto::ed::private_key_shim::data_type;
   data_type buf;
   unpack(s, buf);
   sk = crypto::ed::private_key_shim(buf);
}

}} // namespace fc::raw

namespace fc::crypto::ed {

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const crypto::ed::public_key_shim& pk) {
   ds.write(reinterpret_cast<const char*>(pk._data.data()), crypto_sign_PUBLICKEYBYTES);
   return ds;
}

template <typename DataStream>
DataStream& operator>>(DataStream& ds, crypto::ed::public_key_shim& pk) {
   ds.read(reinterpret_cast<char*>(pk._data.data()), crypto_sign_PUBLICKEYBYTES);
   return ds;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const crypto::ed::signature_shim& sig) {
   ds.write(reinterpret_cast<const char*>(sig._data.data()), crypto_sign_BYTES);
   return ds;
}

template <typename DataStream>
DataStream& operator>>(DataStream& ds, crypto::ed::signature_shim& sig) {
   ds.read(reinterpret_cast<char*>(sig._data.data()), crypto_sign_BYTES);
   return ds;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const crypto::ed::private_key_shim& sk) {
   ds.write(reinterpret_cast<const char*>(sk._data.data()), crypto_sign_SECRETKEYBYTES);
   return ds;
}

template <typename DataStream>
DataStream& operator>>(DataStream& ds, crypto::ed::private_key_shim& sk) {
   ds.read(reinterpret_cast<char*>(sk._data.data()), crypto_sign_SECRETKEYBYTES);
   return ds;
}

} // namespace fc::crypto::ed
