#pragma once
#include <fc/crypto/bigint.hpp>
#include <fc/crypto/common.hpp>
#include <fc/crypto/openssl.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/fwd.hpp>
#include <fc/array.hpp>
#include <fc/io/raw_fwd.hpp>
#include <fc/crypto/elliptic.hpp>
#include <fc/static_variant.hpp>
#include <string>


namespace fc {

  namespace em {
    namespace detail
    {
      class public_key_impl;
      class private_key_impl;
    }

    constexpr uint8_t public_key_prefix_uncompressed = 0x04;
    constexpr uint8_t public_key_prefix_compressed = 0x02;

    using message_hash_type = std::array<uint8_t,32>;
    using message_body_type = std::variant<std::string,fc::sha256,std::vector<uint8_t>>;

    using public_key_data = fc::array<char,33>;
    using public_key_data_uncompressed = fc::array<char,65>;
    using private_key_secret = fc::sha256;

    using signature = fc::array<char,72>;
    using compact_signature = fc::array<unsigned char,65>;

    /**
     *  @class public_key
     *  @brief contains only the public point of an elliptic curve key.
     */
    class public_key
    {
        public:
           public_key();
           public_key(const public_key& k);
           ~public_key();
           public_key_data serialize()const;
           public_key_data_uncompressed serialize_uncompressed()const;

           explicit operator public_key_data()const { return serialize(); }

           explicit public_key( const public_key_data& v );
           explicit public_key( const public_key_data_uncompressed& v );
           public_key( const compact_signature& c, const fc::sha256& digest, bool check_canonical = true );
           public_key( const compact_signature& c, const unsigned char* digest, bool check_canonical = true );

           bool valid()const;

           public_key( public_key&& pk ) noexcept;
           public_key& operator=( public_key&& pk ) noexcept;
           public_key& operator=( const public_key& pk );

           inline friend bool operator==( const public_key& a, const public_key& b )
           {
            return a.serialize() == b.serialize();
           }
           inline friend bool operator!=( const public_key& a, const public_key& b )
           {
            return a.serialize() != b.serialize();
           }

           /// Allows to convert current public key object into base58 number.
           std::string to_base58() const;
           static std::string to_base58( const public_key_data &key );
           static public_key from_base58( const std::string& b58 );

           unsigned int fingerprint() const;
           static bool is_canonical( const compact_signature& c );

        private:
          friend class private_key;
          static public_key from_key_data( const public_key_data& v );
          fc::fwd<detail::public_key_impl,33> my;
    };

    /**
     *  @class private_key
     *  @brief an elliptic curve private key.
     */
    class private_key
    {
        public:
           private_key();
           private_key( private_key&& pk ) noexcept;
           private_key( const private_key& pk );
           ~private_key();

           private_key& operator=( private_key&& pk ) noexcept;
           private_key& operator=( const private_key& pk );

           static private_key generate();
           static private_key regenerate( const fc::sha256& secret );

           private_key child( const fc::sha256& offset )const;

           /**
            *  This method of generation enables creating a new private key in a deterministic manner relative to
            *  an initial seed.   A public_key created from the seed can be multiplied by the offset to calculate
            *  the new public key without having to know the private key.
            */
           static private_key generate_from_seed( const fc::sha256& seed, const fc::sha256& offset = fc::sha256() );

           private_key_secret get_secret()const; // get the private key secret

           explicit operator private_key_secret ()const { return get_secret(); }

           /**
            *  Given a public key, calculatse a 512 bit shared secret between that
            *  key and this private key.
            */
           fc::sha512 get_shared_secret( const public_key& pub )const;

           compact_signature sign_compact( const fc::sha256& digest, bool require_canonical = true )const;

           compact_signature sign_compact_ex(const message_body_type& digest,
                                             bool                                       require_canonical) const;

           public_key get_public_key()const;

           inline friend bool operator==( const private_key& a, const private_key& b )
           {
            return a.get_secret() == b.get_secret();
           }
           inline friend bool operator!=( const private_key& a, const private_key& b )
           {
            return a.get_secret() != b.get_secret();
           }
           inline friend bool operator<( const private_key& a, const private_key& b )
           {
            return a.get_secret() < b.get_secret();
           }

           unsigned int fingerprint() const { return get_public_key().fingerprint(); }

        private:
           fc::fwd<detail::private_key_impl,32> my;
    };

      /**
       * Shims
       */
      struct public_key_shim : public crypto::shim<public_key_data> {
         using crypto::shim<public_key_data>::shim;

         bool valid()const {
            return public_key(_data).valid();
         }

         public_key unwrapped()const { return public_key(_data); }
      };

      struct signature_shim : public crypto::shim<compact_signature> {
         static constexpr bool is_recoverable = true;
         using public_key_type = public_key_shim;
         using crypto::shim<compact_signature>::shim;

         public_key_type recover(const sha256& digest, bool check_canonical) const;
         public_key_type recover_ex(const em::message_body_type& digest, bool check_canonical) const;
      };

      struct private_key_shim : public crypto::shim<private_key_secret> {
         using crypto::shim<private_key_secret>::shim;
         using signature_type = signature_shim;
         using public_key_type = public_key_shim;

         signature_type sign( const sha256& digest, bool require_canonical = true ) const
         {
           return signature_type(private_key::regenerate(_data).sign_compact(digest, require_canonical));
         }

         public_key_type get_public_key( ) const
         {
           return public_key_type(private_key::regenerate(_data).get_public_key().serialize());
         }

         sha512 generate_shared_secret( const public_key_type &pub_key ) const
         {
           return private_key::regenerate(_data).get_shared_secret(public_key(pub_key.serialize()));
         }

         static private_key_shim generate()
         {
            return private_key_shim(private_key::generate().get_secret());
         }

      };

  } // namespace em
  void to_variant( const em::private_key& var,  variant& vo );
  void from_variant( const variant& var,  em::private_key& vo );
  void to_variant( const em::public_key& var,  variant& vo );
  void from_variant( const variant& var,  em::public_key& vo );

  namespace raw
  {
      template<typename Stream>
      void unpack( Stream& s, em::public_key& pk)
      {
          em::public_key_data ser;
          fc::raw::unpack(s,ser);
          pk = em::public_key( ser );
      }

      template<typename Stream>
      void pack( Stream& s, const em::public_key& pk)
      {
          fc::raw::pack( s, pk.serialize() );
      }

      template<typename Stream>
      void unpack( Stream& s, em::private_key& pk)
      {
          fc::sha256 sec;
          unpack( s, sec );
          pk = em::private_key::regenerate(sec);
      }

      template<typename Stream>
      void pack( Stream& s, const em::private_key& pk)
      {
          fc::raw::pack( s, pk.get_secret() );
      }

  } // namespace raw

} // namespace fc
#include <fc/reflect/reflect.hpp>

FC_REFLECT_TYPENAME( fc::em::private_key )
FC_REFLECT_TYPENAME( fc::em::public_key )
FC_REFLECT_DERIVED( fc::em::public_key_shim, (fc::crypto::shim<fc::em::public_key_data>), BOOST_PP_SEQ_NIL )
FC_REFLECT_DERIVED( fc::em::signature_shim, (fc::crypto::shim<fc::em::compact_signature>), BOOST_PP_SEQ_NIL )
FC_REFLECT_DERIVED( fc::em::private_key_shim, (fc::crypto::shim<fc::em::private_key_secret>), BOOST_PP_SEQ_NIL )
