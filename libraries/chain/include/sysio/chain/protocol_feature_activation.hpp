#pragma once

#include <sysio/chain/types.hpp>

namespace sysio { namespace chain {

struct protocol_feature_activation : fc::reflect_init {
   static constexpr uint16_t extension_id() { return 0; }
   static constexpr bool     enforce_unique() { return true; }

   protocol_feature_activation() = default;

   protocol_feature_activation( const vector<digest_type>& pf )
   :protocol_features( pf )
   {}

   protocol_feature_activation( vector<digest_type>&& pf )
   :protocol_features( std::move(pf) )
   {}

   protocol_feature_activation(const protocol_feature_activation&) = default;
   protocol_feature_activation(protocol_feature_activation&&) = default;

   protocol_feature_activation& operator=(protocol_feature_activation&&) = default;
   protocol_feature_activation& operator=(const protocol_feature_activation&) = default;

   void reflector_init();

   vector<digest_type> protocol_features;
};

struct protocol_feature_activation_set;

using protocol_feature_activation_set_ptr = std::shared_ptr<protocol_feature_activation_set>;

struct protocol_feature_activation_set {
   flat_set<digest_type> protocol_features;

   protocol_feature_activation_set() = default;

   protocol_feature_activation_set( const protocol_feature_activation_set& orig_pfa_set,
                                    vector<digest_type> additional_features,
                                    bool  enforce_disjoint = true
                                  );
};


} } // namespace sysio::chain

FC_REFLECT(sysio::chain::protocol_feature_activation,     (protocol_features))
FC_REFLECT(sysio::chain::protocol_feature_activation_set, (protocol_features))
