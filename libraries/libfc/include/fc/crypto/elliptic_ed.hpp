#pragma once

#include <cstring>               // for memset
#include <ostream>               // for std::ostream
#include <sodium.h>              // for ED25519 methods
#include <fc/array.hpp>          // for fc::array
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/sha512.hpp>  // for generate_shared_secret return type
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
   using data_type = fc::array<unsigned char, size>;
   data_type _data;

   public_key_shim() { memset(_data.data, 0, size); }
   explicit public_key_shim(const data_type& d): _data(d) {}

   bool valid() const {
      for(auto b : _data.data) if(b) return true;
      return false;
   }

   data_type serialize() const { return _data; }
};

/**
 * ED25519 signature (64 bytes, padded to 65 for fc::signature compatibility)
 */
struct signature_shim {
   static constexpr size_t size = crypto_sign_BYTES + 1; // 65
   using data_type = fc::array<unsigned char, size>;
   data_type _data;

   signature_shim() { memset(_data.data, 0, size); }
   explicit signature_shim(const data_type& d): _data(d) {}

   data_type serialize() const { return _data; }

   public_key_shim recover(const sha256&, bool) const {
      FC_THROW_EXCEPTION(exception, "ED25519 signature recovery not supported");
   }

   bool verify(const sha256& digest, const public_key_shim& pub) const;
};

/**
 * ED25519 private key (64 bytes secret key from libsodium)
 */
struct private_key_shim {
   static constexpr size_t size = crypto_sign_SECRETKEYBYTES;
   using data_type = fc::array<unsigned char, size>;
   data_type _data;

   private_key_shim() { memset(_data.data, 0, size); }
   explicit private_key_shim(const data_type& d): _data(d) {}

   using public_key_type = public_key_shim;

   public_key_shim get_public_key() const;
   signature_shim  sign(const sha256& digest, bool require_canonical) const;
   sha512          generate_shared_secret(const public_key_shim&) const;

   data_type serialize() const { return _data; }
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

namespace fc {

// sha256::encoder overloads
inline sha256::encoder& operator<<(sha256::encoder& e, const crypto::ed::public_key_shim& pk) {
   e.write(reinterpret_cast<const char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
   return e;
}

inline sha256::encoder& operator<<(sha256::encoder& e, const crypto::ed::signature_shim& sig) {
   e.write(reinterpret_cast<const char*>(sig._data.data), crypto_sign_BYTES);
   return e;
}

// datastream<> overloads
template<typename Storage, typename Enable>
inline datastream<Storage, Enable>& operator<<(datastream<Storage, Enable>& ds, const crypto::ed::public_key_shim& pk) {
   ds.write(reinterpret_cast<const char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
   return ds;
}

template<typename Storage, typename Enable>
inline datastream<Storage, Enable>& operator>>(datastream<Storage, Enable>& ds, crypto::ed::public_key_shim& pk) {
   ds.read(reinterpret_cast<char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
   return ds;
}

template<typename Storage, typename Enable>
inline datastream<Storage, Enable>& operator<<(datastream<Storage, Enable>& ds, const crypto::ed::signature_shim& sig) {
   ds.write(reinterpret_cast<const char*>(sig._data.data), crypto_sign_BYTES);
   return ds;
}

template<typename Storage, typename Enable>
inline datastream<Storage, Enable>& operator>>(datastream<Storage, Enable>& ds, crypto::ed::signature_shim& sig) {
   ds.read(reinterpret_cast<char*>(sig._data.data), crypto_sign_BYTES);
   sig._data.data[crypto_sign_BYTES] = 0;
   return ds;
}

// std::ostream overloads
inline std::ostream& operator<<(std::ostream& os, const crypto::ed::public_key_shim& pk) {
   os.write(reinterpret_cast<const char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
   return os;
}

inline std::ostream& operator<<(std::ostream& os, const crypto::ed::signature_shim& sig) {
   os.write(reinterpret_cast<const char*>(sig._data.data), crypto_sign_BYTES);
   return os;
}

// let raw::unpack(std::istream&, public_key_shim&) work:
inline std::istream&
operator>>(std::istream& is, crypto::ed::public_key_shim& pk) {
   is.read(reinterpret_cast<char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
   return is;
}

// let raw::unpack(std::istream&, signature_shim&) work:
inline std::istream&
operator>>(std::istream& is, crypto::ed::signature_shim& sig) {
   is.read(reinterpret_cast<char*>(sig._data.data), crypto_sign_BYTES);
   // pad the extra byte to zero so serialize() remains correct
   sig._data.data[crypto_sign_BYTES] = 0;
   return is;
}

inline cfile_datastream&
operator>>( cfile_datastream& ds, crypto::ed::public_key_shim& pk ) {
  ds.read(reinterpret_cast<char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
  return ds;
}

inline cfile_datastream&
operator>>( cfile_datastream& ds, crypto::ed::signature_shim& sig ) {
  ds.read(reinterpret_cast<char*>(sig._data.data), crypto_sign_BYTES);
  sig._data.data[crypto_sign_BYTES] = 0;
  return ds;
}

// datastream<> overloads for ED private key shim
template<typename Storage, typename Enable>
inline datastream<Storage,Enable>& operator<<( datastream<Storage,Enable>& ds
                                             , const crypto::ed::private_key_shim& sk ) {
   ds.write(reinterpret_cast<const char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
   return ds;
}

template<typename Storage, typename Enable>
inline datastream<Storage,Enable>& operator>>( datastream<Storage,Enable>& ds
                                             , crypto::ed::private_key_shim& sk ) {
   ds.read(reinterpret_cast<char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
   return ds;
}

// support for in‐memory mb_datastream<S>
template<uint32_t S>
inline mb_datastream<S>& operator<<( mb_datastream<S>& ds, const crypto::ed::public_key_shim& pk ) {
    ds.write(reinterpret_cast<const char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
    return ds;
}
template<uint32_t S>
inline mb_datastream<S>& operator>>( mb_datastream<S>& ds, crypto::ed::public_key_shim& pk ) {
    ds.read(reinterpret_cast<char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
    return ds;
}
template<uint32_t S>
inline mb_datastream<S>& operator<<( mb_datastream<S>& ds, const crypto::ed::signature_shim& sig ) {
    ds.write(reinterpret_cast<const char*>(sig._data.data), crypto_sign_BYTES);
    return ds;
}
template<uint32_t S>
inline mb_datastream<S>& operator>>( mb_datastream<S>& ds, crypto::ed::signature_shim& sig ) {
    ds.read(reinterpret_cast<char*>(sig._data.data), crypto_sign_BYTES);
    // ensure the padding byte is zero
    sig._data.data[crypto_sign_BYTES] = 0;
    return ds;
}
template<uint32_t S>
inline mb_datastream<S>& operator<<( mb_datastream<S>& ds, const crypto::ed::private_key_shim& sk ) {
    ds.write(reinterpret_cast<const char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
    return ds;
}
template<uint32_t S>
inline mb_datastream<S>& operator>>( mb_datastream<S>& ds, crypto::ed::private_key_shim& sk ) {
    ds.read(reinterpret_cast<char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
    return ds;
}

// mb_peek_datastream<> overloads for ED public key shim
template<uint32_t S>
inline mb_peek_datastream<S>& operator>>(mb_peek_datastream<S>& ds, crypto::ed::public_key_shim& pk) {
    ds.read(reinterpret_cast<char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
    return ds;
}

// mb_peek_datastream<> overloads for ED signature shim
template<uint32_t S>
inline mb_peek_datastream<S>& operator>>(mb_peek_datastream<S>& ds, crypto::ed::signature_shim& sig) {
    ds.read(reinterpret_cast<char*>(sig._data.data), crypto_sign_BYTES);
    // Pad the extra byte
    sig._data.data[crypto_sign_BYTES] = 0;
    return ds;
}

// mb_peek_datastream<> overloads for ED private key shim
template<uint32_t S>
inline mb_peek_datastream<S>& operator>>(mb_peek_datastream<S>& ds, crypto::ed::private_key_shim& sk) {
    ds.read(reinterpret_cast<char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
    return ds;
}

// TODO: Optional?
// mb_peek_datastream<> overloads for ED public key shim (write)
template<uint32_t S>
inline mb_peek_datastream<S>& operator<<(mb_peek_datastream<S>& ds, const crypto::ed::public_key_shim& pk) {
    ds.write(reinterpret_cast<const char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
    return ds;
}

// mb_peek_datastream<> overloads for ED signature shim (write)
template<uint32_t S>
inline mb_peek_datastream<S>& operator<<(mb_peek_datastream<S>& ds, const crypto::ed::signature_shim& sig) {
    ds.write(reinterpret_cast<const char*>(sig._data.data), crypto_sign_BYTES);
    return ds;
}

// mb_peek_datastream<> overloads for ED private key shim (write)
template<uint32_t S>
inline mb_peek_datastream<S>& operator<<(mb_peek_datastream<S>& ds, const crypto::ed::private_key_shim& sk) {
    ds.write(reinterpret_cast<const char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
    return ds;
}
} // namespace fc
