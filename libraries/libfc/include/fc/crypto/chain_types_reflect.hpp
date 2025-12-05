#pragma once
#include <cstdint>
#include <fc-lite/crypto/chain_types.hpp>
#include <fc/reflect/reflect.hpp>

/** Add reflection for `chain_kind_t` */
FC_REFLECT_ENUM_WITH_STRIP(fc::crypto::chain_kind_t,
   (chain_kind_unknown)
   (chain_kind_wire)
   (chain_kind_ethereum)
   (chain_kind_solana)
   (chain_kind_sui), true);

/** Add reflection for `chain_key_type_t` */
FC_REFLECT_ENUM_WITH_STRIP(fc::crypto::chain_key_type_t,
   (chain_key_type_unknown)
   (chain_key_type_wire)
   (chain_key_type_ethereum)
   (chain_key_type_solana)
   (chain_key_type_sui), true);


namespace fc::crypto {
   using chain_key_type_reflector = fc::reflector<chain_key_type_t>;
   using chain_kind_reflector = fc::reflector<chain_kind_t>;
}