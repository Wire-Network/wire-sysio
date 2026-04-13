#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/raw.hpp>

namespace sysio::trace_api {

   std::tuple<fc::variant, std::optional<fc::variant>> abi_data_handler::serialize_to_variant(const std::variant<action_trace_v0>& action) {
      return std::visit([&](const auto& a) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         if (!_abi_lookup_fn) {
            return {};
         }

         auto abi_bytes = _abi_lookup_fn(a.account, a.global_sequence);
         if (!abi_bytes || abi_bytes->empty()) {
            return {};
         }

         try {
            chain::abi_def abi;
            auto ds = fc::datastream<const char*>(abi_bytes->data(), abi_bytes->size());
            fc::raw::unpack(ds, abi);

            chain::abi_serializer serializer(std::move(abi),
               chain::abi_serializer::create_yield_function(fc::microseconds::maximum()));

            auto type_name = serializer.get_action_type(a.action);
            if (type_name.empty()) {
               return {};
            }

            // abi_serializer expects a yield function that takes a recursion depth
            // abis are user provided, do not use a deadline
            auto abi_yield = [](size_t recursion_depth) {
               SYS_ASSERT( recursion_depth < chain::abi_serializer::max_recursion_depth,
                           chain::abi_recursion_depth_exception,
                           "exceeded max_recursion_depth {} ", chain::abi_serializer::max_recursion_depth );
            };

            std::optional<fc::variant> ret_data;
            auto params = serializer.binary_to_variant(type_name, a.data, abi_yield);
            if (a.return_value.size() > 0) {
               auto return_type_name = serializer.get_action_result_type(a.action);
               if (!return_type_name.empty()) {
                  ret_data = serializer.binary_to_variant(return_type_name, a.return_value, abi_yield);
               }
            }
            return {std::move(params), std::move(ret_data)};
         } catch (...) {
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         }

         return {};
      }, action);
   }
}
