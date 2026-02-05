#pragma once
#include <fc-lite/crypto/chain_types.hpp>
#include <fc/reflect/reflect.hpp>

/** Add reflection for `chain_kind_t` */
FC_REFLECT_ENUM_WITH_STRIP(fc::crypto::chain_kind_t,
   (unknown)
   (wire)
   (ethereum)
   (solana)
   (sui), true);

/** Add reflection for `chain_key_type_t` */
FC_REFLECT_ENUM_WITH_STRIP(fc::crypto::chain_key_type_t,
   (unknown)
   (wire)
   (wire_bls)
   (ethereum)
   (solana)
   (sui), true);


namespace fc::crypto {
   using chain_key_type_reflector = fc::reflector<chain_key_type_t>;
   using chain_kind_reflector = fc::reflector<chain_kind_t>;
}