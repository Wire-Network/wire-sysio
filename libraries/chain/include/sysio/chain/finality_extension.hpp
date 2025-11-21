#pragma   once

#include <sysio/chain/finalizer_policy.hpp>
#include <sysio/chain/proposer_policy.hpp>
#include <sysio/chain/finality_core.hpp>

namespace sysio::chain {

struct finality_extension : fc::reflect_init {
   static constexpr uint16_t extension_id()   { return 3; }
   static constexpr bool     enforce_unique() { return true; }

   finality_extension() = default;
   finality_extension(qc_claim_t qc_claim,
                              std::optional<finalizer_policy_diff>&& new_finalizer_policy_diff,
                              std::optional<proposer_policy_diff>&& new_proposer_policy_diff) :
      qc_claim(qc_claim),
      new_finalizer_policy_diff(std::move(new_finalizer_policy_diff)),
      new_proposer_policy_diff(std::move(new_proposer_policy_diff))
   {}

   void reflector_init() const {
      static_assert( fc::raw::has_feature_reflector_init_on_unpacked_reflected_types,
                     "finality_extension expects FC to support reflector_init" );
      static_assert( extension_id() == 3, "finality_extension extension id must be 3" );
   }

   qc_claim_t                              qc_claim;
   std::optional<finalizer_policy_diff>    new_finalizer_policy_diff;
   std::optional<proposer_policy_diff>     new_proposer_policy_diff;
};

} /// sysio::chain

FC_REFLECT( sysio::chain::finality_extension, (qc_claim)(new_finalizer_policy_diff)(new_proposer_policy_diff) )
