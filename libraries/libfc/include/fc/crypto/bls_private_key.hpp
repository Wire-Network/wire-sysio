#pragma once

#include <fc/crypto/common.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/reflect/reflect.hpp>
#include <memory>
#include <ranges>

/**
 * @brief Namespace for BLS cryptographic functionality
 */
namespace fc::crypto::bls {

/**
 * @brief Class representing a BLS private key
 */
class private_key {
public:
   /** @brief Default constructor */
   private_key() = default;
   /** @brief Move constructor */
   private_key(private_key&&) = default;
   /** @brief Copy constructor */
   private_key(const private_key&) = default;
   /** @brief Constructs a private key from a secret */
   explicit private_key(const private_key_secret& sk) : _sk(sk) {}

   /** 
    * @brief Constructs a private key from a seed
    * @param seed Byte span containing the seed data
    */
   explicit private_key(std::span<const uint8_t> seed) {
      _sk = bls12_381::secret_key(seed);
   }

   /**
    * @brief Constructs a private key from a base64url encoded string
    * @param base64urlstr The base64url encoded string
    */
   explicit private_key(const std::string& base64urlstr);

   private_key& operator=(const private_key& pk) = default;
   private_key& operator=(private_key&& pk) noexcept = default;

   private_key_secret get_secret() const { return _sk; };
   std::string to_string() const;

   public_key get_public_key() const;

   bls::signature sign(const fc::sha256& digest) const;
   bls::signature sign(std::span<const uint8_t> msg) const;
   bls::signature proof_of_possession() const;

   static private_key generate();

private:
   private_key_secret _sk{};
   friend bool operator==(const private_key& pk1, const private_key& pk2);
   friend struct reflector<private_key>;
}; // private_key

/**
 * Shims
 */
/**
 * @brief Shim class for BLS public key operations
 */
struct public_key_shim {
   using data_type = public_key_data;

   /** @brief Checks if the public key is valid */
   bool valid() const { return public_key(shim_ptr->_data).valid(); }

   /** @brief Unwraps the public key data into a public_key object */
   public_key unwrapped() const { return public_key(shim_ptr->_data); }

   std::string to_string() const { return public_key::to_string(shim_ptr->_data); }

   const data_type& serialize() const {
      return shim_ptr->_data;
   }

   public_key_shim() : shim_ptr(std::make_shared<shim<data_type>>()) {}
   public_key_shim(const public_key_shim& other) = default;
   public_key_shim& operator=(const public_key_shim& other) = default;
   public_key_shim(public_key_shim&& other) noexcept = default;
   public_key_shim& operator=(public_key_shim&& other) noexcept = default;

   public_key_shim(const data_type& data)
      :shim_ptr(std::make_shared<shim<data_type>>(data))
   {}

   public_key_shim& operator=(const data_type& other) {
      shim_ptr->_data = other;
      return *this;
   }

   std::shared_ptr<shim<data_type>> shim_ptr;
};

/**
 * @brief Shim class for BLS signature operations
 */
struct signature_shim {
   using data_type = compact_signature;

   /** @brief Indicates if signature is recoverable */
   static constexpr bool is_recoverable = false;
   /** @brief Type alias for public key shim */
   using public_key_type = public_key_shim;

   signature_shim() : shim_ptr(std::make_shared<shim<data_type>>()) {}
   signature_shim(const signature_shim& other) = default;
   signature_shim& operator=(const signature_shim& other) = default;
   signature_shim(signature_shim&& other) noexcept = default;
   signature_shim& operator=(signature_shim&& other) noexcept = default;

   signature_shim(const data_type& data)
      :shim_ptr(std::make_shared<shim<data_type>>(data))
   {}

   signature_shim& operator=(const data_type& other) {
      shim_ptr->_data = other;
      return *this;
   }

   /**
    * @brief Recovers the public key from a signature and message digest
    * @param digest The message digest that was signed
    * @param check_canonical Whether to verify signature canonicality
    * @return The recovered public key
    */
   public_key_type recover(const sha256& digest, bool check_canonical) const;

   const data_type& serialize() const {
      return shim_ptr->_data;
   }

   /**
    * @brief Unwraps the compact signature into a full BLS signature
    * @return The unwrapped BLS signature
    */
   bls::signature unwrapped() const {
      return bls::signature(shim_ptr->_data);
   };

   /**
    * @brief Converts the signature to string format
    * @return String representation of the signature
    */
   std::string to_string() const {
      return bls::signature::to_string(shim_ptr->_data);
   }

   std::shared_ptr<shim<data_type>> shim_ptr;
};

/**
 * @brief Shim class for BLS private key operations
 */
struct private_key_shim : crypto::shim<private_key_secret> {
   using crypto::shim<private_key_secret>::shim;
   /** @brief Type alias for signature shim */
   using signature_type = signature_shim;
   /** @brief Type alias for public key shim */
   using public_key_type = public_key_shim;

   /**
    * @brief Signs a message digest using this private key
    * @param digest The message digest to sign
    * @param require_canonical Whether to enforce canonical signatures
    * @return The generated signature
    */
   signature_type sign(const sha256& digest, bool require_canonical = false) const {
      return signature_type(private_key(_data).sign(digest).serialize());
   }

   /**
    * @brief Unwraps the private key shim into a full BLS private key
    * @return The unwrapped BLS private key
    */
   bls::private_key unwrapped() const { return bls::private_key(_data); }

   /**
    * @brief Gets the corresponding public key
    * @return The public key derived from this private key
    */
   public_key_type get_public_key() const {
      auto priv_key = unwrapped();
      auto pub_key = priv_key.get_public_key();
      bls::public_key_shim pub_key_shim(pub_key.serialize());
      return pub_key_shim;
   }

   /**
    * @brief Generates a shared secret (not supported for BLS)
    * @param pub_key The public key to generate shared secret with
    * @return The generated shared secret
    * @throws fc::unsupported_exception BLS does not support shared secrets
    */
   sha512 generate_shared_secret(const public_key_type& pub_key) const;

   /**
    * @brief Converts the private key to string format
    * @return String representation of the private key
    */
   std::string to_string() const { return unwrapped().to_string(); }

   /**
    * @brief Generates a new random private key
    * @return The generated private key
    */
   static private_key_shim generate() { return private_key_shim(private_key::generate().get_secret()); }
};


} // namespace fc::crypto::bls

namespace fc {
/**
 * @brief Converts a BLS private key to a variant
 * @param var The private key to convert
 * @param vo The output variant
 */
void to_variant(const crypto::bls::private_key& var, variant& vo);

/**
 * @brief Converts a variant to a BLS private key
 * @param var The variant to convert
 * @param vo The output private key
 */
void from_variant(const variant& var, crypto::bls::private_key& vo);
} // namespace fc


#include <fc/reflect/reflect.hpp>
FC_REFLECT(fc::crypto::bls::private_key, (_sk))

FC_REFLECT_TYPENAME(fc::crypto::bls::public_key)

FC_REFLECT(fc::crypto::bls::public_key_shim, (shim_ptr))
FC_REFLECT(fc::crypto::bls::signature_shim, (shim_ptr))

FC_REFLECT_DERIVED(fc::crypto::bls::private_key_shim, (fc::crypto::shim<fc::crypto::bls::private_key_secret>),
                   BOOST_PP_SEQ_NIL)