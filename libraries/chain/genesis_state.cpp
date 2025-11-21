#include <sysio/chain/genesis_state.hpp>

// these are required to serialize a genesis_state

namespace sysio { namespace chain {

genesis_state::genesis_state() {
   initial_timestamp = fc::time_point::from_iso_string( "2018-06-01T12:00:00" );
   initial_key = fc::variant(sysio_root_key).as<public_key_type>();
   initial_finalizer_key = fc::variant(sysio_root_finalizer_key).as<fc::crypto::blslib::bls_public_key>();
}

chain::chain_id_type genesis_state::compute_chain_id() const {
   digest_type::encoder enc;
   fc::raw::pack( enc, *this );
   return chain_id_type{enc.result()};
}

} } // namespace sysio::chain
