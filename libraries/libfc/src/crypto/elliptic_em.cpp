#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/openssl.hpp>

#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/fwd_impl.hpp>

#include <fc-lite/traits.hpp>

#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <openssl/rand.h>


namespace fc::em {
namespace detail {
const secp256k1_context* _get_context();
void _init_lib();

class private_key_impl {
public:
   private_key_impl() BOOST_NOEXCEPT;
   private_key_impl(const private_key_impl& cpy) BOOST_NOEXCEPT;

   private_key_impl& operator=(const private_key_impl& pk) BOOST_NOEXCEPT;

   private_key_secret _key;
};

const secp256k1_context* _get_context() {
   static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
   return ctx;
}

void _init_lib() {
   static const secp256k1_context* ctx = _get_context();
   (void)ctx;
}

class public_key_impl {
public:
   public_key_impl() noexcept {
      _init_lib();
   }

   public_key_impl(const public_key_impl& cpy) noexcept
      : _key(cpy._key) {
      _init_lib();
   }

   public_key_data _key{};
};

} // namespace detail

static const public_key_data empty_pub{};

public_key::public_key() = default;
public_key::public_key(const public_key& pk) = default;
public_key::public_key(public_key&& pk) noexcept = default;
public_key::~public_key() = default;
public_key& public_key::operator=(const public_key& pk) = default;
public_key& public_key::operator=(public_key&& pk) noexcept = default;

bool operator==(const public_key& a, const public_key& b) {
   return a.my->_key == b.my->_key;
}

bool public_key::valid() const {
   return my->_key != empty_pub;
}

public_key_data public_key::serialize() const {
   FC_ASSERT(my->_key != empty_pub);
   return my->_key;
}

public_key_data_uncompressed public_key::serialize_uncompressed(const public_key_data& key) {
   public_key_data_uncompressed pubkey_data{};
   secp256k1_pubkey pubkey{};

   FC_ASSERT(secp256k1_ec_pubkey_parse(
                detail::_get_context(),
                &pubkey,
                reinterpret_cast<const unsigned char*>(key.data()),
                key.size()
             ), "Invalid public key data");

   size_t pubkey_len = pubkey_data.size();
   FC_ASSERT(secp256k1_ec_pubkey_serialize(
                detail::_get_context(),
                reinterpret_cast<unsigned char*>(pubkey_data.data()),
                &pubkey_len,
                &pubkey,
                SECP256K1_EC_UNCOMPRESSED
             ), "Failed to serialize public key");

   return pubkey_data;
}

public_key_data_uncompressed public_key::serialize_uncompressed() const {
   return serialize_uncompressed(my->_key);
}

public_key::public_key(const public_key_data_uncompressed& dat) {
   const char* front = &dat[0];
   if (*front == 0) {
      return;
   }
   secp256k1_pubkey pubkey{};

   FC_ASSERT(secp256k1_ec_pubkey_parse(
                detail::_get_context(),
                &pubkey,
                reinterpret_cast<const unsigned char*>(dat.data()),
                dat.size()
             ), "Invalid public key data");

   size_t pubkey_len = my->_key.size();
   FC_ASSERT(secp256k1_ec_pubkey_serialize(
                detail::_get_context(),
                reinterpret_cast<unsigned char*>(my->_key.data()),
                &pubkey_len,
                &pubkey,
                SECP256K1_EC_COMPRESSED
             ), "Failed to serialize public key");

   FC_ASSERT(pubkey_len == my->_key.size(), "Invalid public key size");
}

public_key::public_key(const public_key_data& dat) {
   my->_key = dat;
}

public_key public_key::recover(const compact_signature& c, const fc::sha256& digest, bool check_canonical) {
   return recover(c, keccak256::hash(digest.to_uint8_span()).data(), check_canonical);
}

public_key public_key::recover(const compact_signature& c, const unsigned char* digest, bool check_canonical) {
   public_key result;
   int nV = c[c.size() - 1];
   if (nV < 27 || nV >= 35) {
      FC_THROW_EXCEPTION(exception, "unable to reconstruct public key from signature");
   }

   if (check_canonical) {
      FC_ASSERT(is_canonical(c), "signature is not canonical");
   }

   // Declare the necessary secp256k1 variables for public key and signature
   secp256k1_pubkey secp_pub{};
   secp256k1_ecdsa_recoverable_signature secp_sig{};

   // Parse the compact signature into a recoverable signature
   auto c_last = c.end() - 1;
   FC_ASSERT(secp256k1_ecdsa_recoverable_signature_parse_compact(
      detail::_get_context(),
      &secp_sig,
      c.begin(),
      (*c_last - 27) & 3));

   // Recover the public key from the signature and digest
   FC_ASSERT(secp256k1_ecdsa_recover(
      detail::_get_context(),
      &secp_pub,
      &secp_sig,
      digest));

   // Prepare to serialize the recovered public key
   size_t serialized_result_sz = result.my->_key.size();
   secp256k1_ec_pubkey_serialize(
      detail::_get_context(),
      (unsigned char*)&result.my->_key[0],
      &serialized_result_sz,
      &secp_pub,
      SECP256K1_EC_COMPRESSED);

   // Verify the serialized public key size
   FC_ASSERT(serialized_result_sz == result.my->_key.size());
   return result;
}

public_key public_key::from_key_data(const public_key_data& data) {
   return public_key(data);
}

public_key public_key::from_string(const std::string& pub_key_str) {
   return crypto::ethereum::to_em_public_key(pub_key_str);
}

bool public_key::is_canonical(const compact_signature& c) {
   // This is the secp256k1 curve order 'n' divided by 2
   static const unsigned char half_order[32] = {
      0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0x5D, 0x57, 0x6E, 0x73, 0x57, 0xA4, 0x50, 0x1D,
      0xDF, 0xE9, 0x2F, 0x46, 0x68, 0x1B, 0x20, 0xA0
   };

   static_assert(sizeof(c) >= 65, "compact_signature must be 65 bytes");

   // The S value is the 32 bytes starting at index 32.
   // We check if S <= half_order.
   // memcmp returns <= 0 if S is less than or equal to half_order.
   return memcmp(c.data() + 32, half_order, 32) <= 0;
}

//
// private_key
//

private_key private_key::from_native_string(const std::string& priv_key_str) {
   return crypto::ethereum::to_em_private_key(priv_key_str);
}

private_key private_key::generate() {
   const secp256k1_context* ctx = detail::_get_context();

   // Use fc::sha256 as the 32-byte container for our new secret
   private_key_secret new_secret{};

   // Loop until we find a valid key. (An invalid key is 0 or >= the curve order)
   while (true) {
      // Fill the fc::sha256 object with 32 random bytes.
      if (RAND_bytes(reinterpret_cast<uint8_t*>(new_secret.data()), fc::data_size(new_secret)) != 1) {
         FC_THROW_EXCEPTION(exception, "Failed to get random bytes from RAND_bytes");
      }

      // Check if the key is valid using libsecp256k1.
      if (secp256k1_ec_seckey_verify(ctx, reinterpret_cast<const uint8_t*>(new_secret.data())) == 1) {
         break; // valid key
      }
   }

   return regenerate(new_secret);
}

namespace detail {

private_key_impl::private_key_impl() noexcept : _key{} {
   _init_lib();
}

private_key_impl::private_key_impl(const private_key_impl& cpy) noexcept {
   _init_lib();
   this->_key = cpy._key;
}

private_key_impl& private_key_impl::operator=(const private_key_impl& pk) noexcept {
   _key = pk._key;
   return *this;
}

} // namespace detail

static const private_key_secret empty_priv{};

private_key::private_key() = default;
private_key::private_key(const private_key& pk) = default;
private_key::private_key(private_key&& pk) noexcept = default;
private_key::~private_key() = default;
private_key& private_key::operator=(private_key&& pk) noexcept = default;
private_key& private_key::operator=(const private_key& pk) = default;

private_key private_key::regenerate(const private_key_secret& secret) {
   private_key self;
   self.my->_key = secret;
   return self;
}

const private_key_secret& private_key::get_secret() const {
   return my->_key;
}

public_key private_key::get_public_key() const {
   FC_ASSERT(my->_key != empty_priv);
   public_key_data pub{};
   size_t pub_len = sizeof(pub);
   secp256k1_pubkey secp_pub{};
   FC_ASSERT(secp256k1_ec_pubkey_create( detail::_get_context(), &secp_pub, (unsigned char*) my->_key.data() ));
   secp256k1_ec_pubkey_serialize(detail::_get_context(), (unsigned char*)&pub, &pub_len, &secp_pub,
                                 SECP256K1_EC_COMPRESSED);
   FC_ASSERT(pub_len == pub.size());
   return public_key(pub);
}

fc::em::compact_signature private_key::sign_compact(const fc::keccak256& digest) const {
   // Sign the Keccak-256 hash ---
   const secp256k1_context* ctx = detail::_get_context();
   fc::em::compact_signature result;
   secp256k1_ecdsa_recoverable_signature secp_sig;

   if (secp256k1_ecdsa_sign_recoverable(
          ctx,
          &secp_sig,
          digest.data(), // <-- We sign the Keccak-256 hash
          reinterpret_cast<const unsigned char*>(&my->_key), // The 32-byte private key
          nullptr,
          nullptr
          ) != 1) {
      FC_THROW_EXCEPTION(exception, "Failed to sign Keccak-256 digest with libsecp256k1");
   }

   int recovery_id;

   secp256k1_ecdsa_recoverable_signature_serialize_compact(
      ctx,
      result.data(),
      &recovery_id,
      &secp_sig
      );

   result[64] = 27 + recovery_id; // V

   return result;
}

signature_shim::public_key_type signature_shim::recover(const sha256& digest) const {
   // Wire transaction signed by MetaMask: apply EIP-191 prefix before recovery
   auto eth_digest = crypto::ethereum::hash_user_message(digest.to_uint8_span());
   return recover_eth(eth_digest);
}

signature_shim::public_key_type signature_shim::recover_eth(const keccak256& digest) const {
   return public_key_type(public_key::recover(_data, digest.data(), false).serialize());
}

private_key_shim::signature_type private_key_shim::sign_sha256(const sha256& digest) const {
   // Wire transaction signing via MetaMask: apply EIP-191 prefix before signing
   auto eth_digest = crypto::ethereum::hash_user_message(digest.to_uint8_span());
   return sign_keccak256(eth_digest);
}

} // namespace fc::em

namespace fc {
void to_variant(const em::private_key& var, variant& vo) {
   vo = var.get_secret();
}

void from_variant(const variant& var, em::private_key& vo) {
   em::private_key_secret sec{};
   from_variant(var, sec);
   vo = em::private_key::regenerate(sec);
}

void to_variant(const em::public_key& var, variant& vo) {
   vo = var.serialize();
}

void from_variant(const variant& var, em::public_key& vo) {
   em::public_key_data dat{};
   from_variant(var, dat);
   vo = em::public_key(dat);
}

} // namespace fc