#include <fc/crypto/elliptic_em.hpp>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/hmac.hpp>
#include <fc/crypto/openssl.hpp>
#include <fc/crypto/sha512.hpp>

#include <fc/fwd_impl.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>

#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <openssl/rand.h>

#include "_elliptic_em_impl_priv.hpp"

namespace fc::em {
namespace detail {

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

   public_key_data _key;
};

} // namespace detail

static const public_key_data empty_pub;

fc::sha512 private_key::get_shared_secret(const public_key& other) const {
   static const private_key_secret empty_priv;
   FC_ASSERT(my->_key != empty_priv);
   FC_ASSERT(other.my->_key != empty_pub);
   secp256k1_pubkey secp_pubkey;
   FC_ASSERT(secp256k1_ec_pubkey_parse( detail::_get_context(), &secp_pubkey, (unsigned char*)other.serialize().data, other.serialize().size() ));
   FC_ASSERT(secp256k1_ec_pubkey_tweak_mul( detail::_get_context(), &secp_pubkey, (unsigned char*) my->_key.data() ));
   public_key_data serialized_result;
   size_t serialized_result_sz = sizeof(serialized_result);
   secp256k1_ec_pubkey_serialize(detail::_get_context(), (unsigned char*)&serialized_result.data, &serialized_result_sz, &secp_pubkey, SECP256K1_EC_COMPRESSED);
   FC_ASSERT(serialized_result_sz == sizeof(serialized_result));
   return fc::sha512::hash(serialized_result.begin() + 1, serialized_result.size() - 1);
}


public_key::public_key() = default;
public_key::public_key( const public_key &pk ) = default;
public_key::public_key( public_key &&pk ) noexcept = default;
public_key::~public_key() = default;
public_key& public_key::operator=( const public_key& pk ) = default;
public_key& public_key::operator=( public_key&& pk ) noexcept = default;

bool public_key::valid() const {
   return my->_key != empty_pub;
}

std::string public_key::to_base58() const {
   FC_ASSERT(my->_key != empty_pub);
   return to_base58(my->_key);
}

public_key_data public_key::serialize() const {
   FC_ASSERT(my->_key != empty_pub);
   return my->_key;
}

public_key::public_key( const public_key_point_data& dat ) {
  const char* front = &dat.data[0];
  if( *front == 0 ) {
  } else {
      EC_KEY *key = EC_KEY_new_by_curve_name( NID_secp256k1 );
      key = o2i_ECPublicKey( &key, (const unsigned char**)&front, sizeof(dat) );
      FC_ASSERT( key );
      EC_KEY_set_conv_form( key, POINT_CONVERSION_COMPRESSED );
      unsigned char* buffer = (unsigned char*) my->_key.begin();
      i2o_ECPublicKey( key, &buffer ); // FIXME: questionable memory handling
      EC_KEY_free( key );
  }
}

public_key::public_key( const public_key_data& dat ) {
  my->_key = dat;
}

public_key::public_key( const compact_signature& c, const fc::sha256& digest, bool check_canonical ) {
  int nV = c.data[0];
  if (nV<27 || nV>=35)
      FC_THROW_EXCEPTION( exception, "unable to reconstruct public key from signature" );

  if( check_canonical ) {
      FC_ASSERT( is_canonical( c ), "signature is not canonical" );
  }

  secp256k1_pubkey secp_pub;
  secp256k1_ecdsa_recoverable_signature secp_sig;

  FC_ASSERT( secp256k1_ecdsa_recoverable_signature_parse_compact( detail::_get_context(), &secp_sig, (unsigned char*)c.begin() + 1, (*c.begin() - 27) & 3) );
  FC_ASSERT( secp256k1_ecdsa_recover( detail::_get_context(), &secp_pub, &secp_sig, (unsigned char*) digest.data() ) );

  size_t serialized_result_sz = my->_key.size();
  secp256k1_ec_pubkey_serialize( detail::_get_context(), (unsigned char*)&my->_key.data, &serialized_result_sz, &secp_pub, SECP256K1_EC_COMPRESSED );
  FC_ASSERT( serialized_result_sz == my->_key.size() );
}

public_key::public_key(const compact_signature& c, const unsigned char* digest, bool check_canonical) {
  int nV = c.data[0];
  if (nV < 27 || nV >= 35) {
      FC_THROW_EXCEPTION(exception, "unable to reconstruct public key from signature");
  }

  if (check_canonical) {
      FC_ASSERT(is_canonical(c), "signature is not canonical");
  }

  // Declare the necessary secp256k1 variables for public key and signature
  secp256k1_pubkey secp_pub;
  secp256k1_ecdsa_recoverable_signature secp_sig;

  // Parse the compact signature into a recoverable signature
  FC_ASSERT(secp256k1_ecdsa_recoverable_signature_parse_compact(
      detail::_get_context(),
      &secp_sig,
      (unsigned char*)c.begin() + 1,
      (*c.begin() - 27) & 3));

  // Recover the public key from the signature and digest
  FC_ASSERT(secp256k1_ecdsa_recover(
      detail::_get_context(),
      &secp_pub,
      &secp_sig,
      digest));

  // Prepare to serialize the recovered public key
  size_t serialized_result_sz = my->_key.size();
  secp256k1_ec_pubkey_serialize(
      detail::_get_context(),
      (unsigned char*)&my->_key.data,
      &serialized_result_sz,
      &secp_pub,
      SECP256K1_EC_COMPRESSED);

  // Verify the serialized public key size
  FC_ASSERT(serialized_result_sz == my->_key.size());
}

public_key public_key::from_key_data( const public_key_data& data ) {
   return public_key(data);
}

private_key private_key::child( const fc::sha256& offset )const {
   fc::sha256::encoder enc;
   fc::raw::pack(enc, get_public_key());
   fc::raw::pack(enc, offset);
   return generate_from_seed(get_secret(), enc.result());
}

std::string public_key::to_base58( const public_key_data& key ) {
   uint32_t check = (uint32_t)sha256::hash(key.data, sizeof(key))._hash[0];
   static_assert(sizeof(key) + sizeof(check) == 37, ""); // hack around gcc bug: key.size() should be constexpr, but isn't
   array<char, 37> data;
   memcpy(data.data, key.begin(), key.size());
   memcpy(data.begin() + key.size(), (const char*)&check, sizeof(check));
   return fc::to_base58(data.begin(), data.size(), fc::yield_function_t());
}

public_key public_key::from_base58( const std::string& b58 ) {
   array<char, 37> data;
   size_t s = fc::from_base58(b58, (char*)&data, sizeof(data));
   FC_ASSERT(s == sizeof(data));

   public_key_data key;
   uint32_t check = (uint32_t)sha256::hash(data.data, sizeof(key))._hash[0];
   FC_ASSERT(memcmp( (char*)&check, data.data + sizeof(key), sizeof(check) ) == 0);
   memcpy((char*)key.data, data.data, sizeof(key));
   return from_key_data(key);
}

unsigned int public_key::fingerprint() const {
   public_key_data key = serialize();
   ripemd160 hash = ripemd160::hash(sha256::hash(key.begin(), key.size()));
   unsigned char* fp = (unsigned char*)hash._hash;
   return (fp[0] << 24) | (fp[1] << 16) | (fp[2] << 8) | fp[3];
}

bool public_key::is_canonical(const compact_signature& c) {
   // This is the secp256k1 curve order 'n' divided by 2
   static const unsigned char half_order[32] = {
      0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0x5D, 0x57, 0x6E, 0x73, 0x57, 0xA4, 0x50, 0x1D,
      0xDF, 0xE9, 0x2F, 0x46, 0x68, 0x1B, 0x20, 0xA0
   };

   static_assert(sizeof(c.data) >= 65, "compact_signature must be 65 bytes");

   // The S value is the 32 bytes starting at index 33.
   // We check if S <= half_order.
   // memcmp returns <= 0 if S is less than or equal to half_order.
   return memcmp(c.data + 33, half_order, 32) <= 0;
}

//
// private_key
//

private_key private_key::generate_from_seed(const fc::sha256& seed, const fc::sha256& offset) {
   ssl_bignum z;
   BN_bin2bn((unsigned char*)&offset, sizeof(offset), z);

   ec_group group(EC_GROUP_new_by_curve_name(NID_secp256k1));
   bn_ctx ctx(BN_CTX_new());
   ssl_bignum order;
   EC_GROUP_get_order(group, order, ctx);

   // secexp = (seed + z) % order
   ssl_bignum secexp;
   BN_bin2bn((unsigned char*)&seed, sizeof(seed), secexp);
   BN_add(secexp, secexp, z);
   BN_mod(secexp, secexp, order, ctx);

   fc::sha256 secret;
   FC_ASSERT(BN_num_bytes(secexp) <= int64_t(sizeof(secret)));
   auto shift = sizeof(secret) - BN_num_bytes(secexp);
   BN_bn2bin(secexp, ((unsigned char*)&secret) + shift);
   return regenerate(secret);
}

private_key private_key::generate() {
   const secp256k1_context* ctx = detail::_get_context();

   // Use fc::sha256 as the 32-byte container for our new secret
   fc::sha256 new_secret;

   // Loop until we find a valid key. (An invalid key is 0 or >= the curve order)
   while (true) {
      // Fill the fc::sha256 object with 32 random bytes.
      if (RAND_bytes(reinterpret_cast<uint8_t*>(new_secret.data()), new_secret.data_size()) != 1) {
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

private_key_impl::private_key_impl() noexcept {
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

// "x19Ethereum Signed Message:\n" is the header used by the `eth_sign` RPC call. We have converted the string to its hex value to save a step.
// std::string erc155_message_prefix = "19457468657265756d205369676e6564204d6573736167653a0a3332";
constexpr uint8_t eth_prefix[28] = {
   25, 69, 116, 104, 101, 114, 101,
   117, 109, 32, 83, 105, 103, 110,
   101, 100, 32, 77, 101, 115, 115,
   97, 103, 101, 58, 10, 51, 50
};

} // namespace detail

static const private_key_secret empty_priv;

private_key::private_key() = default;

private_key::private_key(const private_key& pk) = default;

private_key::private_key(private_key&& pk) noexcept = default;

private_key::~private_key() = default;

private_key& private_key::operator=(private_key&& pk) noexcept = default;

private_key& private_key::operator=(const private_key& pk) = default;

private_key private_key::regenerate(const fc::sha256& secret) {
   private_key self;
   self.my->_key = secret;
   return self;
}

fc::sha256 private_key::get_secret() const {
   return my->_key;
}

public_key private_key::get_public_key() const {
   FC_ASSERT(my->_key != empty_priv);
   public_key_data pub;
   size_t pub_len = sizeof(pub);
   secp256k1_pubkey secp_pub;
   FC_ASSERT(secp256k1_ec_pubkey_create( detail::_get_context(), &secp_pub, (unsigned char*) my->_key.data() ));
   secp256k1_ec_pubkey_serialize(detail::_get_context(), (unsigned char*)&pub, &pub_len, &secp_pub, SECP256K1_EC_COMPRESSED);
   FC_ASSERT(pub_len == pub.size());
   return public_key(pub);
}

/**
 * Creates a compact, recoverable signature formatted for eth_sign.
 *
 * @param digest The 32-byte *original message hash* (e.g., keccak256(message))
 */
fc::ecc::compact_signature private_key::sign_compact(const fc::sha256& digest, bool require_canonical) const {
   // --- 1. Prepare the eth_sign prefixed hash (same as in recover) ---

   // "\x19Ethereum Signed Message:\n32"

   // Concatenate prefix and the digest
   unsigned char eth_prefixed_msg_raw[28 + 32];
   std::copy(std::begin(detail::eth_prefix), std::end(detail::eth_prefix), std::begin(eth_prefixed_msg_raw));
   std::copy((unsigned char*)digest.data(), (unsigned char*)digest.data() + 32,
             std::begin(eth_prefixed_msg_raw) + 28);

   // Hash the 60-byte prefixed message
   SHA3_CTX msg_ctx;
   keccak_init(&msg_ctx);
   keccak_update(&msg_ctx, eth_prefixed_msg_raw, 60);

   // This is the actual 32-byte hash we need to sign
   unsigned char msg_digest_result[32];
   keccak_final(&msg_ctx, msg_digest_result);

   // --- 2. Sign the new prefixed hash (msg_digest_result) ---

   const secp256k1_context* ctx = detail::_get_context();
   secp256k1_ecdsa_recoverable_signature sig;

   if (secp256k1_ecdsa_sign_recoverable(
          ctx,
          &sig,
          msg_digest_result, // <-- We sign the prefixed hash
          reinterpret_cast<const unsigned char*>(&my->_key), // The 32-byte private key
          NULL,
          NULL
          ) != 1) {
      FC_THROW_EXCEPTION(exception, "Failed to sign prefixed digest with libsecp256k1");
   }

   // --- 3. Serialize the signature (same as before) ---
   unsigned char r_and_s[64];
   int recovery_id = 0;

   secp256k1_ecdsa_recoverable_signature_serialize_compact(
      ctx,
      r_and_s,
      &recovery_id,
      &sig
      );

   fc::ecc::compact_signature compact_sig;
   compact_sig.data[0] = 27 + recovery_id; // V
   memcpy(compact_sig.data + 1, r_and_s, 64); // R and S

   return compact_sig;
}

signature_shim::public_key_type signature_shim::recover(const sha256& digest, bool check_canonical) const {
   // Hash (keccak256) the msg string
   unsigned char eth_prefixed_msg_raw[28 + 32];
   std::copy(std::begin(detail::eth_prefix), std::end(detail::eth_prefix), std::begin(eth_prefixed_msg_raw));
   std::copy((unsigned char*)digest.data(), (unsigned char*)digest.data() + 32, std::begin(eth_prefixed_msg_raw) + 28);

   SHA3_CTX msg_ctx;
   keccak_init(&msg_ctx);
   keccak_update(&msg_ctx, eth_prefixed_msg_raw, 60);

   // Hash the newly created raw message.
   unsigned char msg_digest_result[32];
   keccak_final(&msg_ctx, msg_digest_result);

   return public_key_type(public_key(_data, msg_digest_result, false).serialize());
}

} // namespace fc::em

namespace fc {
void to_variant( const em::private_key& var,  variant& vo )
{
   vo = var.get_secret();
}

void from_variant( const variant& var,  em::private_key& vo )
{
   fc::sha256 sec;
   from_variant( var, sec );
   vo = em::private_key::regenerate(sec);
}

void to_variant( const em::public_key& var,  variant& vo )
{
   vo = var.serialize();
}

void from_variant( const variant& var,  em::public_key& vo )
{
   em::public_key_data dat;
   from_variant( var, dat );
   vo = em::public_key(dat);
}

} // namespace fc
