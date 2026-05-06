#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/json_stream.hpp>

namespace sysio::trace_api {

   namespace {
      // ABIs are user-provided; the yield function only enforces the recursion-depth cap and
      // intentionally has no wall-clock deadline.  Shared between the variant and streaming paths
      // so they stay in lock-step on what counts as "too deep".
      auto make_abi_yield() {
         return [](size_t recursion_depth) {
            SYS_ASSERT( recursion_depth < chain::abi_serializer::max_recursion_depth, chain::abi_recursion_depth_exception,
                        "exceeded max_recursion_depth {} ", chain::abi_serializer::max_recursion_depth );
         };
      }
   }

   void abi_data_handler::add_abi( const chain::name& name, chain::abi_def&& abi ) {
      // currently abis are operator provided so no need to protect against abuse
      abi_serializer_by_account.emplace(name,
            std::make_shared<chain::abi_serializer>(std::move(abi), chain::abi_serializer::create_yield_function(fc::microseconds::maximum())));
   }

   std::tuple<fc::variant, std::optional<fc::variant>> abi_data_handler::serialize_to_variant(const std::variant<action_trace_v0>& action) {
      return std::visit([&](const auto& a) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         auto it = abi_serializer_by_account.find(a.account);
         if (it == abi_serializer_by_account.end()) return {};
         const auto& serializer_p = it->second;
         auto type_name = serializer_p->get_action_type(a.action);
         if (type_name.empty()) return {};

         auto abi_yield = make_abi_yield();

         // Each field is decoded independently.  A parse failure on one (truncated bytes, type
         // mismatch, etc.) is logged but does not prevent the other from being attempted.  The one
         // exception is abi_recursion_depth_exception: the ABI itself is structurally bad, retrying
         // the other field with the same ABI cannot succeed in a useful way, so bail entirely with
         // both fields empty.  Deadline exceptions are not raised here -- this yield does not carry
         // a deadline.
         fc::variant params;
         try {
            params = serializer_p->binary_to_variant(type_name, a.data, abi_yield);
         } catch (const chain::abi_recursion_depth_exception&) {
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
            return {};
         } catch (...) {
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
            // params stays default-constructed (null variant); fall through to return_data.
         }

         std::optional<fc::variant> ret_data;
         if (a.return_value.size() > 0) {
            auto return_type_name = serializer_p->get_action_result_type(a.action);
            if (!return_type_name.empty()) {
               try {
                  ret_data = serializer_p->binary_to_variant(return_type_name, a.return_value, abi_yield);
               } catch (...) {
                  // No "next" field after return_data, so abi_recursion_depth_exception is treated
                  // the same as any other failure: log and leave ret_data nullopt.  params remains.
                  except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
               }
            }
         }

         return {std::move(params), std::move(ret_data)};
      }, action);
   }

   void abi_data_handler::serialize_to_json_stream(const std::variant<action_trace_v0>& action, fc::json_writer& w) {
      std::visit([&](const auto& a) {
         auto it = abi_serializer_by_account.find(a.account);
         if (it == abi_serializer_by_account.end()) return;
         const auto& serializer_p = it->second;
         auto type_name = serializer_p->get_action_type(a.action);
         if (type_name.empty()) return;

         auto abi_yield = make_abi_yield();

         // Same independence rule as serialize_to_variant: a parse failure on one field is logged
         // but does not prevent the other from being attempted; abi_recursion_depth_exception is
         // the lone short-circuit case (structurally bad ABI -- retrying the other field cannot
         // help).  Each field has its own json_writer::checkpoint() / rewind() so a partial emit
         // is rolled back to keep the writer balanced.
         {
            auto cp = w.checkpoint();
            try {
               w.key("params");
               serializer_p->binary_to_json_stream(type_name, a.data, w, abi_yield);
            } catch (const chain::abi_recursion_depth_exception&) {
               w.rewind(cp);
               except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
               return;  // bail; do not attempt return_data
            } catch (...) {
               w.rewind(cp);
               except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
               // fall through and attempt return_data independently
            }
         }

         if (a.return_value.empty()) return;
         auto return_type_name = serializer_p->get_action_result_type(a.action);
         if (return_type_name.empty()) return;

         auto cp = w.checkpoint();
         try {
            w.key("return_data");
            serializer_p->binary_to_json_stream(return_type_name, a.return_value, w, abi_yield);
         } catch (...) {
            // No "next" field after return_data, so abi_recursion_depth_exception is treated the
            // same as any other failure: rewind, log, leave params (if any) intact.
            w.rewind(cp);
            except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
         }
      }, action);
   }
}
