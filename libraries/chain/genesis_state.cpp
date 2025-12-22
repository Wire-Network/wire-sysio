#include <sysio/chain/genesis_state.hpp>

// these are required to serialize a genesis_state

namespace sysio { namespace chain {

genesis_state::genesis_state(const fc::crypto::public_key& producer_key, const fc::crypto::public_key& finalizer_key) {
   initial_timestamp = fc::time_point::from_iso_string( "2018-06-01T12:00:00" );
   initial_key = producer_key;
   initial_finalizer_key = finalizer_key.get<fc::crypto::bls::public_key_shim>().unwrapped();
}

chain::chain_id_type genesis_state::compute_chain_id() const {
   digest_type::encoder enc;
   fc::raw::pack( enc, *this );
   return chain_id_type{enc.result()};
}

} } // namespace sysio::chain
