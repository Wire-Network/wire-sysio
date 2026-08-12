#pragma once

#include <atomic>
#include <fc/exception/exception.hpp>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <utility>
#include <variant>

namespace sysio::batch_operator_detail {

namespace push_action_log {
inline constexpr auto success = "batch_operator: pushed {}::{} ok";
inline constexpr auto failure = "batch_operator: push {}::{} failed: {}";
} // namespace push_action_log

/// Owns completion notification for an asynchronous batch-operator action.
///
/// The state is intentionally shared with the completion callback so a caller
/// may time out and destroy its future without leaving the callback with a
/// dangling promise or permitting a second completion to throw.
class async_action_completion {
public:
   /// Returns the single future that waits for this action's callback.
   /// This may be called exactly once for each completion instance.
   std::future<void> get_future() { return done.get_future(); }

   /// Invokes `on_complete` once, suppresses callback exceptions, and signals
   /// the waiter without allowing duplicate asynchronous completions to throw.
   template <typename CompletionHandler>
   bool complete(CompletionHandler&& on_complete) {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
         return false;
      }

      try {
         std::forward<CompletionHandler>(on_complete)();
      } catch (...) {
         // A logging or formatting failure must not escape the relay callback.
      }

      try {
         done.set_value();
      } catch (...) {
         // The one-shot guard prevents future_error in normal operation.
      }
      return true;
   }

   /// Dispatches a push-transaction result to the matching logging handler
   /// before completing the waiter. The result is consumed synchronously, so
   /// only the handlers need ownership that survives the asynchronous callback.
   template <typename SuccessHandler, typename FailureHandler>
   bool complete_push_result(
      const chain::next_function_variant<chain_apis::read_write::push_transaction_results>& result,
      SuccessHandler&& on_success,
      FailureHandler&& on_failure) {
      return complete([&result, &on_success, &on_failure] {
         if (const auto* error = std::get_if<fc::exception_ptr>(&result)) {
            on_failure(*error);
         } else if (std::holds_alternative<chain_apis::read_write::push_transaction_results>(result)) {
            on_success();
         } else {
            const auto& deferred = std::get<std::function<
               chain::t_or_exception<chain_apis::read_write::push_transaction_results>()>>(result);
            const auto deferred_result = deferred();
            if (const auto* error = std::get_if<fc::exception_ptr>(&deferred_result)) {
               on_failure(*error);
            } else {
               on_success();
            }
         }
      });
   }

private:
   std::atomic<bool> completed{false};
   std::promise<void> done;
};

/// Creates the transaction callback while retaining its API through deferred completion.
inline chain::plugin_interface::next_function<chain_apis::read_write::push_transaction_results>
create_push_action_callback(std::shared_ptr<void> api_lifetime,
      std::shared_ptr<async_action_completion> completion,
      std::string contract,
      std::string action_name) {
   return [api_lifetime = std::move(api_lifetime),
           completion = std::move(completion),
           contract = std::move(contract),
           action_name = std::move(action_name)](const auto& result) {
      // `read_write::push_transaction` retains a raw `this` in its own callback.
      (void)api_lifetime;
      completion->complete_push_result(
         result,
         [&contract, &action_name] {
            ilog(push_action_log::success, contract, action_name);
         },
         [&contract, &action_name](const fc::exception_ptr& error) {
            elog(push_action_log::failure, contract, action_name, error->to_string());
         });
   };
}

} // namespace sysio::batch_operator_detail
