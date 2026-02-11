#include <fc/crypto/elliptic.hpp>

#include <fc/crypto/hmac.hpp>
#include <fc/crypto/openssl.hpp>
#include <fc/crypto/rand.hpp>

#include <fc/fwd_impl.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>

#include <fc-lite/traits.hpp>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <openssl/rand.h>

#if _WIN32
# include <malloc.h>
#elif defined(__FreeBSD__)
# include <stdlib.h>
#else
# include <alloca.h>
#endif

#include "_elliptic_impl_priv.hpp"

namespace fc::ecc {
    namespace detail
    {
        struct context_creator {
           context_creator() {
              ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
              char seed[32];
              rand_bytes(seed, sizeof(seed));
              FC_ASSERT(secp256k1_context_randomize(ctx, (const unsigned char*)seed));
           }
           secp256k1_context* ctx = nullptr;
        };
        const secp256k1_context* _get_context() {
            static context_creator cc;
            return cc.ctx;
        }

        class public_key_impl {
        public:
           public_key_impl() BOOST_NOEXCEPT = default;
           public_key_impl(const public_key_impl& cpy) BOOST_NOEXCEPT = default;
           public_key_impl(public_key_impl&& cpy) BOOST_NOEXCEPT = default;

           public_key_data _key{};

           public_key_impl& operator=(const public_key_impl& pk) BOOST_NOEXCEPT = default;
           public_key_impl& operator=(public_key_impl&& pk) BOOST_NOEXCEPT = default;
        };

        typedef std::array<char,37> chr37;
        chr37 _derive_message( const public_key_data& key, int i );
        fc::sha256 _left( const fc::sha512& v );
        fc::sha256 _right( const fc::sha512& v );
        const ec_group& get_curve();
        const private_key_secret& get_curve_order();
        const private_key_secret& get_half_curve_order();
    }

    static const public_key_data empty_pub{};
    

    private_key private_key::generate()
    {
       private_key ret;
       do {
         rand_bytes(reinterpret_cast<char*>(ret.my->_key.data()), fc::data_size(ret.my->_key));
       } while(!secp256k1_ec_seckey_verify(detail::_get_context(), (const uint8_t*)ret.my->_key.data()));
       return ret;
    }

    public_key::public_key() = default;
    public_key::public_key( const public_key& pk ) = default;
    public_key::public_key( public_key &&pk ) noexcept = default;
    public_key::~public_key() = default;

    public_key& public_key::operator=( const public_key& pk ) = default;
    public_key& public_key::operator=( public_key&& pk ) noexcept = default;

    bool public_key::valid()const
    {
      return my->_key != empty_pub;
    }

    public_key_data public_key::serialize()const
    {
        FC_ASSERT( my->_key != empty_pub );
        return my->_key;
    }

    public_key::public_key( const public_key_point_data& dat )
    {
        auto front = dat.begin();
        if( *front == 0 ){}
        else
        {
            EC_KEY *key = EC_KEY_new_by_curve_name( NID_secp256k1 );
            key = o2i_ECPublicKey( &key, &front, static_cast<long>(dat.size()) );
            FC_ASSERT( key );
            EC_KEY_set_conv_form( key, POINT_CONVERSION_COMPRESSED );
            unsigned char* buffer = (unsigned char*) my->_key.begin();
            i2o_ECPublicKey( key, &buffer ); // FIXME: questionable memory handling
            EC_KEY_free( key );
        }
    }

    public_key::public_key( const public_key_data& dat )
    {
        my->_key = dat;
    }

    public_key public_key::recover( const compact_signature& c, const fc::sha256& digest )
    {
        public_key result;
        int nV = c[0];
        if (nV<27 || nV>=35)
            FC_THROW_EXCEPTION( exception, "unable to reconstruct public key from signature" );

        secp256k1_pubkey secp_pub{};
        secp256k1_ecdsa_recoverable_signature secp_sig{};

        FC_ASSERT( secp256k1_ecdsa_recoverable_signature_parse_compact( detail::_get_context(), &secp_sig, (unsigned char*)c.begin() + 1, (*c.begin() - 27) & 3) );
        FC_ASSERT( secp256k1_ecdsa_recover( detail::_get_context(), &secp_pub, &secp_sig, (unsigned char*) digest.data() ) );

        size_t serialized_result_sz = result.my->_key.size();
        secp256k1_ec_pubkey_serialize( detail::_get_context(), (unsigned char*)&result.my->_key[0], &serialized_result_sz, &secp_pub, SECP256K1_EC_COMPRESSED );
        FC_ASSERT( serialized_result_sz == result.my->_key.size() );
        return result;
    }

} // namespace fc::ecc
