#pragma once

#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
// controller.hpp must precede plugin_interface.hpp: the latter uses block_signal_params
// (declared in controller.hpp) without including it, so this header includes it to stay
// self-contained regardless of the caller's include order.
#include <sysio/chain/controller.hpp>
#include <sysio/chain/plugin_interface.hpp>

#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/time.hpp>

#include <cassert>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace sysio {

/**
 * @brief Compile-time tag selecting the response control flow used by `bind_stream`.
 *
 *  - `sync`        : `R api.method(args...)` -- result is captured directly into the
 *                    json_writer-emitting closure handed to the streaming cb.
 *  - `sync_void`   : `void api.method(args...)` -- emits the convention `{"result":"ok"}`
 *                    response, matching the producer_api / net_api void-method pattern.
 *  - `post`        : `function<t_or_exception<T>()> api.method(args...)` -- Phase 1
 *                    runs on the calling queue and returns a closure (`http_fwd`) that
 *                    will produce the typed result; Phase 2 runs `http_fwd` on the http
 *                    thread pool and emits via `to_json_stream(result)`.
 *  - `post_direct` : `function<t_or_exception<emit_fn_t>()> api.method(args...)` -- like
 *                    `post`, but Phase 2 hands the emit closure straight to the cb.
 *                    Used when the api builds and emits its JSON without an
 *                    intermediate reflected struct (eg per-row binary decode).
 *  - `async`       : `void api.method(args..., next_function<T>)` -- the api invokes
 *                    the callback later with either a typed result, an exception, or a
 *                    Phase-2 `http_fwd` closure (mirroring the variant-cb semantics).
 */
enum class dispatch { sync, sync_void, post, post_direct, async };

namespace http_detail {

   /// Standard `{"result":"ok"}` reply emitted by `dispatch::sync_void`.  Reflected
   /// here so each api plugin doesn't have to define its own ack struct.
   struct ok_response {
      std::string result = "ok";
   };

   using emit_fn_t = std::function<void(fc::json_writer&)>;

   /// @brief Pulls apart a member-function pointer into its pieces.
   ///
   /// Specialized for both non-const and const-qualified member functions so
   /// `member_fn<decltype(&Cls::method)>` works regardless of cv-qualification.
   template<typename M> struct member_fn;

   template<typename R, typename Cls, typename... A>
   struct member_fn<R(Cls::*)(A...)> {
      using class_t = Cls;
      using ret_t   = R;
      template<size_t I> using arg_t = std::tuple_element_t<I, std::tuple<A...>>;
      static constexpr size_t arity = sizeof...(A);
   };

   template<typename R, typename Cls, typename... A>
   struct member_fn<R(Cls::*)(A...) const> {
      using class_t = Cls;
      using ret_t   = R;
      template<size_t I> using arg_t = std::tuple_element_t<I, std::tuple<A...>>;
      static constexpr size_t arity = sizeof...(A);
   };

   /// @brief Detects `function<t_or_exception<T>()>` (the Phase-2 closure shape).
   template<typename T> struct is_typed_fwd : std::false_type {};
   template<typename T>
   struct is_typed_fwd<std::function<chain::t_or_exception<T>()>> : std::true_type {
      using payload = T;
   };

   /// @brief Detects `chain::plugin_interface::next_function<T>` (the async cb shape).
   template<typename T> struct is_next_fn : std::false_type {};
   template<typename T>
   struct is_next_fn<chain::plugin_interface::next_function<T>> : std::true_type {
      using payload = T;
   };

   /// @brief True iff T is `fc::time_point` after stripping cv/ref.
   template<typename T>
   inline constexpr bool is_deadline_v =
      std::is_same_v<std::remove_cvref_t<T>, fc::time_point>;

   /// Lazily evaluates the method's first argument bare-type when `HasArgs` is true.
   /// Required because `std::conditional_t` eagerly instantiates both branches, so a
   /// direct `mf::arg_t<0>` reference fails to compile for arity-0 methods.
   template<typename MF, bool HasArgs>
   struct first_arg_or_void { using type = std::monostate; };
   template<typename MF>
   struct first_arg_or_void<MF, true> {
      using type = std::remove_cvref_t<typename MF::template arg_t<0>>;
   };

   /// Compute the per-request deadline.  Chain api handles expose `start()` which both
   /// returns the deadline and registers the in-flight call; other handles fall back to
   /// `http_plugin::get_max_response_time()`.  Constraint requires `start()` return to
   /// be convertible to fc::time_point so an unrelated `start()` does not match.
   template<typename Handle>
   inline fc::time_point compute_deadline(Handle& handle, http_plugin& http) {
      if constexpr (requires { { handle.start() } -> std::convertible_to<fc::time_point>; }) {
         return handle.start();
      } else if constexpr (requires { { handle->start() } -> std::convertible_to<fc::time_point>; }) {
         return handle->start();
      } else {
         const fc::microseconds m = http.get_max_response_time();
         return m == fc::microseconds::maximum() ? fc::time_point::maximum()
                                                 : fc::time_point::now() + m;
      }
   }

   /// Runtime dispatch wrapper around the compile-time `parse_params<T, pt>` template.
   /// Three instantiations per T -- compilers inline through the switch.
   template<typename T>
   inline T parse_params_rt(const std::string& body, http_params_types pt) {
      switch (pt) {
         case http_params_types::no_params:
            return parse_params<T, http_params_types::no_params>(body);
         case http_params_types::params_required:
            return parse_params<T, http_params_types::params_required>(body);
         case http_params_types::possible_no_params:
            return parse_params<T, http_params_types::possible_no_params>(body);
      }
      std::unreachable();
   }

   /// Parse a registration path of the form "/v1/<api>/<call>" into the
   /// (api_name, call_name) labels used by `handle_exception_stream`.  Returns
   /// owning std::string because the caller captures the labels into a long-lived
   /// registration lambda.
   inline std::pair<std::string, std::string> split_path(std::string_view path) {
      auto last_slash = path.find_last_of('/');
      assert(last_slash != std::string_view::npos && last_slash > 0); // expects "/api/call" shape
      auto prev_slash = path.find_last_of('/', last_slash - 1);
      assert(prev_slash != std::string_view::npos);
      return { std::string(path.substr(prev_slash + 1, last_slash - prev_slash - 1)),
               std::string(path.substr(last_slash + 1)) };
   }

   /// Invoke `MethodPtr` against `handle` with the right argument shape:
   ///   arity 0           -> ()
   ///   arity 1           -> (params)
   ///   arity 2 + deadline-> (params, deadline)
   ///   arity 1 async     -> (next_function<T>)
   ///   arity 2 async     -> (params, next_function<T>)
   /// Deadline-vs-not is selected by inspecting the last param type at compile time.
   template<auto MethodPtr, typename Handle, typename Params>
   inline auto invoke_sync(Handle& handle, Params&& params, fc::time_point deadline) {
      using mf = member_fn<decltype(MethodPtr)>;
      if constexpr (mf::arity == 0) {
         return std::invoke(MethodPtr, handle);
      } else if constexpr (mf::arity == 1) {
         return std::invoke(MethodPtr, handle, std::forward<Params>(params));
      } else {
         // arity == 2: (params, deadline).
         static_assert(is_deadline_v<typename mf::template arg_t<1>>,
                       "second argument must be fc::time_point (deadline)");
         return std::invoke(MethodPtr, handle, std::forward<Params>(params), deadline);
      }
   }

   /// Async variant: last argument is `next_function<T>`, called by api method later.
   template<auto MethodPtr, typename Handle, typename Params, typename Next>
   inline void invoke_async(Handle& handle, Params&& params, Next&& next) {
      using mf = member_fn<decltype(MethodPtr)>;
      if constexpr (mf::arity == 1) {
         // (next_function<T>) -- no params arg, body must be empty
         std::invoke(MethodPtr, handle, std::forward<Next>(next));
      } else {
         // (params, next_function<T>)
         std::invoke(MethodPtr, handle, std::forward<Params>(params), std::forward<Next>(next));
      }
   }

} // namespace http_detail


/**
 *  @brief Build an `api_entry_stream` registration for an api method, dispatching the
 *  response per `Kind`.
 *
 *  @tparam MethodPtr  pointer-to-member of the api class implementing the endpoint
 *  @tparam Kind       which response shape (`dispatch::sync` etc) the method uses;
 *                     compile-time `static_assert`s pin Kind to the method signature
 *                     so a refactor that changes the signature produces a build error,
 *                     not a silent behavior change.
 *  @tparam Handle     the api handle type (read_only, read_write, producer_plugin&,
 *                     plugin*, ...). Captured by-value into the lambda; for handles
 *                     that should be referenced across requests pass an explicit
 *                     reference type or pointer.
 *
 *  @param http        the http_plugin instance, used for the http thread pool, max
 *                     response time, and exception logging.
 *  @param handle      the api handle, captured into the request lambda.
 *  @param path        e.g. "/v1/chain/get_info"; used for url registration AND for
 *                     deriving the (api_name, call_name) labels passed to
 *                     `handle_exception_stream` on errors.
 *  @param cat         api category for the registration.
 *  @param pt          parameter parsing mode for the request body.
 *  @param resp_code   HTTP status code on success.
 *
 *  @return            an `api_entry_stream` ready to be batched into
 *                     `http_plugin::add_api_stream` / `add_async_api_stream`.
 */
template<auto MethodPtr, dispatch Kind, typename Handle>
api_entry_stream bind_stream(http_plugin& http, Handle handle,
                              std::string path, api_category cat,
                              http_params_types pt, uint16_t resp_code) {
   using namespace http_detail;
   using mf       = member_fn<decltype(MethodPtr)>;
   using ret_t    = typename mf::ret_t;

   // ----- Compile-time signature validation against `Kind` ------------------
   if constexpr (Kind == dispatch::sync) {
      static_assert(!std::is_void_v<ret_t>,
         "dispatch::sync expects T return; method returns void (use dispatch::sync_void).");
      static_assert(!is_typed_fwd<ret_t>::value,
         "dispatch::sync method returns function<t_or_exception<T>()>; use dispatch::post or dispatch::post_direct.");
   } else if constexpr (Kind == dispatch::sync_void) {
      static_assert(std::is_void_v<ret_t>,
         "dispatch::sync_void expects void return; method returns a value.");
   } else if constexpr (Kind == dispatch::post) {
      static_assert(is_typed_fwd<ret_t>::value,
         "dispatch::post expects function<t_or_exception<T>()> return.");
      static_assert(!std::is_same_v<typename is_typed_fwd<ret_t>::payload, emit_fn_t>,
         "dispatch::post payload is emit_fn_t; use dispatch::post_direct instead.");
   } else if constexpr (Kind == dispatch::post_direct) {
      static_assert(std::is_same_v<ret_t, std::function<chain::t_or_exception<emit_fn_t>()>>,
         "dispatch::post_direct expects function<t_or_exception<emit_fn_t>()> return.");
   } else { // dispatch::async
      static_assert(std::is_void_v<ret_t>,
         "dispatch::async expects void return.");
      static_assert(mf::arity >= 1, "dispatch::async requires at least one arg (next_function<T>).");
      static_assert(
         is_next_fn<std::remove_cvref_t<typename mf::template arg_t<mf::arity - 1>>>::value,
         "dispatch::async requires the last argument to be next_function<T>.");
   }

   // ----- Parameter type deduction ------------------------------------------
   // `params_t` is the *first* method arg's bare type (when present).
   // For arity-0 sync methods we don't parse a body.
   constexpr bool has_params = []() {
      if constexpr (Kind == dispatch::async) {
         // async signature: (next_function) or (params, next_function)
         return mf::arity >= 2;
      } else {
         return mf::arity >= 1;
      }
   }();

   using params_t = typename http_detail::first_arg_or_void<mf, has_params>::type;

   auto labels = split_path(path);
   std::string api_name  = std::move(labels.first);
   std::string call_name = std::move(labels.second);

   return api_entry_stream {
      std::move(path), cat,
      [handle = std::move(handle), &http, pt, resp_code,
       api_name = std::move(api_name), call_name = std::move(call_name)]
      (std::string&&, std::string&& body, url_response_stream_callback&& cb) mutable {
         // `http` is referenced by some Kinds (post/post_direct/async/sync via start());
         // taking its address unconditionally keeps the capture flagged as used so the
         // compiler does not emit -Wunused-lambda-capture for sync_void instantiations.
         (void)&http;

         // For sync paths that need a deadline, compute it before the try{} so that
         // chain_api's start() side effect runs even if param parsing throws.
         fc::time_point deadline;
         if constexpr (Kind != dispatch::sync_void) {
            deadline = compute_deadline(handle, http);
         }

         try {
            // ---- Parse params (if the method takes any) ----
            // Immediately-invoked initializer so `params` is constructed directly from the
            // parse result (no default-construct-then-assign, and param structs need not be
            // default-constructible).  The no-params arm still validates the body per `pt`
            // and yields the std::monostate placeholder.
            [[maybe_unused]] params_t params = [&]() -> params_t {
               if constexpr (has_params) {
                  return parse_params_rt<params_t>(body, pt);
               } else {
                  (void)parse_params_rt<std::string>(body, pt);
                  return {};
               }
            }();

            // ---- Dispatch on Kind --------------------------------------
            if constexpr (Kind == dispatch::sync) {
               auto result = invoke_sync<MethodPtr>(handle, std::move(params), deadline);
               cb(resp_code, [r = std::move(result)](fc::json_writer& w) mutable {
                  fc::to_json_stream(r, w);
               });

            } else if constexpr (Kind == dispatch::sync_void) {
               if constexpr (mf::arity == 0) {
                  std::invoke(MethodPtr, handle);
               } else {
                  std::invoke(MethodPtr, handle, std::move(params));
               }
               cb(resp_code, [](fc::json_writer& w) {
                  ok_response r;
                  fc::to_json_stream(r, w);
               });

            } else if constexpr (Kind == dispatch::post) {
               using payload_t = typename is_typed_fwd<ret_t>::payload;
               using http_fwd_t = std::function<chain::t_or_exception<payload_t>()>;
               http_fwd_t http_fwd = invoke_sync<MethodPtr>(handle, std::move(params), deadline);
               http.post_http_thread_pool(
                  [resp_code, cb = std::move(cb), body = std::move(body),
                   api_name, call_name,
                   http_fwd = std::move(http_fwd)]() mutable {
                     try {
                        chain::t_or_exception<payload_t> result = http_fwd();
                        if (std::holds_alternative<fc::exception_ptr>(result)) {
                           // rethrow() (virtual) preserves the stored exception's dynamic type so
                           // classify_current_exception's specific-type catches map it to the right
                           // status code (tx_duplicate -> 409, unknown_block -> 400, ...).  A plain
                           // `throw *ptr` would slice to fc::exception and turn them all into 500s.
                           try { std::get<fc::exception_ptr>(result)->rethrow(); }
                           catch (...) {
                              http_plugin::handle_exception_stream(
                                 api_name.c_str(), call_name.c_str(), body, cb);
                           }
                        } else {
                           cb(resp_code,
                              [r = std::get<payload_t>(std::move(result))]
                              (fc::json_writer& w) mutable { fc::to_json_stream(r, w); });
                        }
                     } catch (...) {
                        http_plugin::handle_exception_stream(
                           api_name.c_str(), call_name.c_str(), body, cb);
                     }
                  });

            } else if constexpr (Kind == dispatch::post_direct) {
               using http_fwd_t = std::function<chain::t_or_exception<emit_fn_t>()>;
               http_fwd_t http_fwd = invoke_sync<MethodPtr>(handle, std::move(params), deadline);
               http.post_http_thread_pool(
                  [resp_code, cb = std::move(cb), body = std::move(body),
                   api_name, call_name,
                   http_fwd = std::move(http_fwd)]() mutable {
                     try {
                        chain::t_or_exception<emit_fn_t> result = http_fwd();
                        if (std::holds_alternative<fc::exception_ptr>(result)) {
                           try { std::get<fc::exception_ptr>(result)->rethrow(); }
                           catch (...) {
                              http_plugin::handle_exception_stream(
                                 api_name.c_str(), call_name.c_str(), body, cb);
                           }
                        } else {
                           cb(resp_code, std::get<emit_fn_t>(std::move(result)));
                        }
                     } catch (...) {
                        http_plugin::handle_exception_stream(
                           api_name.c_str(), call_name.c_str(), body, cb);
                     }
                  });

            } else { // dispatch::async
               using payload_t = typename is_next_fn<
                  std::remove_cvref_t<typename mf::template arg_t<mf::arity - 1>>>::payload;
               using http_fwd_t = std::function<chain::t_or_exception<payload_t>()>;
               // The next_function operator() is rvalue-only; move out of the variant into
               // the response closure so the heavy payload (raw ABIs, traces) is moved, not
               // copied, before the closure runs on the http pool.
               static_assert(std::is_move_constructible_v<payload_t>,
                             "bind_stream::async requires the next_function payload to be move-constructible.");

               auto next = [&http, cb = std::move(cb), body = std::move(body),
                            api_name, call_name, resp_code]
                  (chain::plugin_interface::next_function_variant<payload_t>&& result) mutable {
                     if (std::holds_alternative<fc::exception_ptr>(result)) {
                        try { std::get<fc::exception_ptr>(result)->rethrow(); }
                        catch (...) {
                           http_plugin::handle_exception_stream(
                              api_name.c_str(), call_name.c_str(), body, cb);
                        }
                     } else if (std::holds_alternative<payload_t>(result)) {
                        cb(resp_code,
                           [r = std::get<payload_t>(std::move(result))]
                           (fc::json_writer& w) mutable { fc::to_json_stream(r, w); });
                     } else {
                        http.post_http_thread_pool(
                           [resp_code, cb = std::move(cb), body = std::move(body),
                            api_name, call_name,
                            http_fwd = std::get<http_fwd_t>(std::move(result))]() mutable {
                              try {
                                 chain::t_or_exception<payload_t> r = http_fwd();
                                 if (std::holds_alternative<fc::exception_ptr>(r)) {
                                    try { std::get<fc::exception_ptr>(r)->rethrow(); }
                                    catch (...) {
                                       http_plugin::handle_exception_stream(
                                          api_name.c_str(), call_name.c_str(), body, cb);
                                    }
                                 } else {
                                    cb(resp_code,
                                       [v = std::get<payload_t>(std::move(r))]
                                       (fc::json_writer& w) mutable {
                                          fc::to_json_stream(v, w);
                                       });
                                 }
                              } catch (...) {
                                 http_plugin::handle_exception_stream(
                                    api_name.c_str(), call_name.c_str(), body, cb);
                              }
                           });
                     }
                  };

               // Empty body -> default-constructed params for async (matches
               // CALL_ASYNC_STREAM "if (body.empty()) body = "{}";" preflight).
               if constexpr (has_params) {
                  invoke_async<MethodPtr>(handle, std::move(params), std::move(next));
               } else {
                  // Arity-1 async: no params slot, invoke the method directly.
                  std::invoke(MethodPtr, handle, std::move(next));
               }
            }
         } catch (...) {
            http_plugin::handle_exception_stream(
               api_name.c_str(), call_name.c_str(), body, cb);
         }
      }
   };
}

} // namespace sysio

FC_REFLECT(sysio::http_detail::ok_response, (result));
