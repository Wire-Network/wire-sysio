#include <sysio/chain/producer_schedule.hpp>

namespace sysio { namespace chain {

fc::variant producer_authority::get_abi_variant() const {
      auto authority_variant = std::visit([](const auto& a){
         fc::variant value;
         fc::to_variant(a, value);

         fc::variant type = std::string(std::decay_t<decltype(a)>::abi_type_name());

         return fc::variants {
               std::move(type),
               std::move(value)
         };
      }, authority);

      return fc::mutable_variant_object()
            ("producer_name", producer_name)
            ("authority", std::move(authority_variant));
}

} } /// sysio::chain
