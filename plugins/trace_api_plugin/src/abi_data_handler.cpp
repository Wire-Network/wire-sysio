#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>

namespace sysio::trace_api {

   void abi_data_handler::add_abi( const chain::name& name, chain::abi_def&& abi ) {
      // currently abis are operator provided so no need to protect against abuse
      abi_serializer_by_account.emplace(name,
            std::make_shared<chain::abi_serializer>(std::move(abi), chain::abi_serializer::create_yield_function(fc::microseconds::maximum())));
   }

   std::tuple<fc::variant, std::optional<fc::variant>> abi_data_handler::serialize_to_variant(const std::variant<action_trace_v0>& action) {
      return std::visit([&](const auto& a) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         if (abi_serializer_by_account.count(a.account) > 0) {
            const auto &serializer_p = abi_serializer_by_account.at(a.account);
            auto type_name = serializer_p->get_action_type(a.action);

            if (!type_name.empty()) {
               try {
                  // abi_serializer expects a yield function that takes a recursion depth
                  // abis are user provided, do not use a deadline
                  auto abi_yield = [](size_t recursion_depth) {
                     SYS_ASSERT( recursion_depth < chain::abi_serializer::max_recursion_depth, chain::abi_recursion_depth_exception,
                                 "exceeded max_recursion_depth {} ", chain::abi_serializer::max_recursion_depth );
                  };
                  std::optional<fc::variant> ret_data;
                  auto params = serializer_p->binary_to_variant(type_name, a.data, abi_yield);
                  if(a.return_value.size() > 0) {
                     auto return_type_name = serializer_p->get_action_result_type(a.action);
                     if (!return_type_name.empty()) {
                        ret_data = serializer_p->binary_to_variant(return_type_name, a.return_value, abi_yield);
                     }
                  }
                  return {std::move(params), std::move(ret_data)};
               } catch (...) {
                  except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
               }
            }
         }
         return {};
      }, action);
   }
}
