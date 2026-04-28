#pragma once

#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>

#define CALL_ASYNC_WITH_400(api_name, category, api_handle, api_namespace, call_name, call_result, http_resp_code, params_type) \
{ std::string("/v1/" #api_name "/" #call_name),                                                                 \
  api_category::category,                                                                                       \
  [api_handle, &_http_plugin](string&&, string&& body, url_response_callback&& cb) mutable {                    \
     api_handle.start();                                                                                        \
     try {                                                                                                      \
        auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                     \
        using http_fwd_t = std::function<chain::t_or_exception<call_result>()>;                                 \
        api_handle.call_name( std::move(params), /* called on main application thread */                        \
           [&_http_plugin, cb=std::move(cb), body=std::move(body)]                                              \
           (const chain::next_function_variant<call_result>& result) mutable {                                  \
              if (std::holds_alternative<fc::exception_ptr>(result)) {                                          \
                 try {                                                                                          \
                    throw *std::get<fc::exception_ptr>(result);                                                 \
                 } catch (...) {                                                                                \
                    http_plugin::handle_exception(#api_name, #call_name, body, cb);                             \
                 }                                                                                              \
              } else if (std::holds_alternative<call_result>(result)) {                                         \
                 cb(http_resp_code, fc::variant(std::get<call_result>(std::move(result))));                     \
              } else {                                                                                          \
                 /* api returned a function to be processed on the http_plugin thread pool */                   \
                 assert(std::holds_alternative<http_fwd_t>(result));                                            \
                 _http_plugin.post_http_thread_pool([resp_code=http_resp_code, cb=std::move(cb),                \
                                                     body=std::move(body),                                      \
                                                     http_fwd = std::get<http_fwd_t>(std::move(result))]() {    \
                    chain::t_or_exception<call_result> result = http_fwd();                                     \
                    if (std::holds_alternative<fc::exception_ptr>(result)) {                                    \
                       try {                                                                                    \
                          throw *std::get<fc::exception_ptr>(result);                                           \
                       } catch (...) {                                                                          \
                          http_plugin::handle_exception(#api_name, #call_name, body, cb);                       \
                       }                                                                                        \
                    } else {                                                                                    \
                       cb(resp_code, fc::variant(std::get<call_result>(std::move(result))));                    \
                    }                                                                                           \
                 });                                                                                            \
              }                                                                                                 \
           });                                                                                                  \
     } catch (...) {                                                                                            \
        http_plugin::handle_exception(#api_name, #call_name, body, cb);                                         \
     }                                                                                                          \
   }                                                                                                            \
}


// Streaming-cb counterpart of (locally-defined) CALL_WITH_400.  Sync endpoint: call runs on the
// calling queue (typically read_only), result is captured into a json_writer-
// emitting closure that the cb forwards to the http thread pool for emission.
// No fc::variant tree on the response path.
// ------------------------------------------------------------------------------------------------------
#define CALL_WITH_400_STREAM(api_name, category, api_handle, api_namespace, call_name, call_result, http_resp_code, params_type) \
{std::string("/v1/" #api_name "/" #call_name),                                                                  \
      api_category::category,                                                                                   \
      [api_handle](string&&, string&& body, url_response_stream_callback&& cb) mutable {                        \
          auto deadline = api_handle.start();                                                                   \
          try {                                                                                                 \
             auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                \
             call_result result = api_handle.call_name(std::move(params), deadline);                            \
             cb(http_resp_code, [r = std::move(result)](fc::json_writer& w) mutable {                           \
                fc::to_json_stream(r, w);                                                                       \
             });                                                                                                \
          } catch (...) {                                                                                       \
             http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                             \
          }                                                                                                     \
       }}


// Direct-streaming counterpart of CALL_WITH_400_STREAM_POST.  Phase 1 returns a
// closure that, when invoked on the http thread pool (Phase 2), produces the
// final json_writer-emitting closure directly -- no typed `call_result` struct
// on the response path.  Used when the api builds and emits its JSON response
// without an intermediate variant or reflected struct (eg per-row binary decode).
// Method name on api_handle is `<call_name>_stream` so the variant-cb method
// continues to compile alongside.
// ------------------------------------------------------------------------------------------------------
#define CALL_WITH_400_STREAM_POST_DIRECT(api_name, category, api_handle, api_namespace, call_name, http_resp_code, params_type) \
{std::string("/v1/" #api_name "/" #call_name),                                                                  \
      api_category::category,                                                                                   \
      [api_handle, &_http_plugin](string&&, string&& body, url_response_stream_callback&& cb) {                 \
          auto deadline = api_handle.start();                                                                   \
          try {                                                                                                 \
             auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                \
             using emit_fn_t  = std::function<void(fc::json_writer&)>;                                          \
             using http_fwd_t = std::function<chain::t_or_exception<emit_fn_t>()>;                              \
             http_fwd_t http_fwd(api_handle.call_name ## _stream(std::move(params), deadline));                 \
             _http_plugin.post_http_thread_pool([resp_code=http_resp_code, cb=std::move(cb),                    \
                                                 body=std::move(body),                                          \
                                                 http_fwd = std::move(http_fwd)]() mutable {                    \
                try {                                                                                           \
                   chain::t_or_exception<emit_fn_t> result = http_fwd();                                        \
                   if (std::holds_alternative<fc::exception_ptr>(result)) {                                     \
                      try {                                                                                     \
                         throw *std::get<fc::exception_ptr>(result);                                            \
                      } catch (...) {                                                                           \
                         http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                 \
                      }                                                                                         \
                   } else {                                                                                     \
                      cb(resp_code, std::get<emit_fn_t>(std::move(result)));                                    \
                   }                                                                                            \
                } catch (...) {                                                                                 \
                   http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                       \
                }                                                                                               \
             });                                                                                                \
          } catch (...) {                                                                                       \
             http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                             \
          }                                                                                                     \
       }}


// Streaming-cb counterpart of CALL_WITH_400_POST.  Same Phase 1 (api thread) /
// Phase 2 (http thread pool) split as the variant version, but Phase 2 hands
// the typed call_result to cb as a json_writer-emitting closure - no fc::variant
// tree on the response path.
// ------------------------------------------------------------------------------------------------------
#define CALL_WITH_400_STREAM_POST(api_name, category, api_handle, api_namespace, call_name, call_result, http_resp_code, params_type) \
{std::string("/v1/" #api_name "/" #call_name),                                                                  \
      api_category::category,                                                                                   \
      [api_handle, &_http_plugin](string&&, string&& body, url_response_stream_callback&& cb) {                 \
          auto deadline = api_handle.start();                                                                   \
          try {                                                                                                 \
             auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                \
             using http_fwd_t = std::function<chain::t_or_exception<call_result>()>;                            \
             /* called on main application thread */                                                            \
             http_fwd_t http_fwd(api_handle.call_name(std::move(params), deadline));                            \
             _http_plugin.post_http_thread_pool([resp_code=http_resp_code, cb=std::move(cb),                    \
                                                 body=std::move(body),                                          \
                                                 http_fwd = std::move(http_fwd)]() mutable {                    \
                try {                                                                                           \
                   chain::t_or_exception<call_result> result = http_fwd();                                      \
                   if (std::holds_alternative<fc::exception_ptr>(result)) {                                     \
                      try {                                                                                     \
                         throw *std::get<fc::exception_ptr>(result);                                            \
                      } catch (...) {                                                                           \
                         http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                 \
                      }                                                                                         \
                   } else {                                                                                     \
                      cb(resp_code, [r = std::get<call_result>(std::move(result))]                              \
                                    (fc::json_writer& w) mutable { fc::to_json_stream(r, w); });                \
                   }                                                                                            \
                } catch (...) {                                                                                 \
                   http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                       \
                }                                                                                               \
             });                                                                                                \
          } catch (...) {                                                                                       \
             http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                             \
          }                                                                                                     \
       }}


// Streaming-cb counterpart of CALL_ASYNC_WITH_400.  Same control flow; the api
// lambda hands the result struct to the cb as a json_writer-emitting closure
// instead of as a fc::variant.  No per-field allocation on the response path.
// ------------------------------------------------------------------------------------------------------
#define CALL_ASYNC_WITH_400_STREAM(api_name, category, api_handle, api_namespace, call_name, call_result, http_resp_code, params_type) \
{ std::string("/v1/" #api_name "/" #call_name),                                                                 \
  api_category::category,                                                                                       \
  [api_handle, &_http_plugin](string&&, string&& body, url_response_stream_callback&& cb) mutable {             \
     api_handle.start();                                                                                        \
     try {                                                                                                      \
        auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                     \
        using http_fwd_t = std::function<chain::t_or_exception<call_result>()>;                                 \
        api_handle.call_name( std::move(params), /* called on main application thread */                        \
           [&_http_plugin, cb=std::move(cb), body=std::move(body)]                                              \
           (const chain::next_function_variant<call_result>& result) mutable {                                  \
              if (std::holds_alternative<fc::exception_ptr>(result)) {                                          \
                 try {                                                                                          \
                    throw *std::get<fc::exception_ptr>(result);                                                 \
                 } catch (...) {                                                                                \
                    http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                      \
                 }                                                                                              \
              } else if (std::holds_alternative<call_result>(result)) {                                         \
                 cb(http_resp_code, [r = std::get<call_result>(std::move(result))]                              \
                                    (fc::json_writer& w) mutable { fc::to_json_stream(r, w); });                \
              } else {                                                                                          \
                 assert(std::holds_alternative<http_fwd_t>(result));                                            \
                 _http_plugin.post_http_thread_pool([resp_code=http_resp_code, cb=std::move(cb),                \
                                                     body=std::move(body),                                      \
                                                     http_fwd = std::get<http_fwd_t>(std::move(result))]() mutable { \
                    chain::t_or_exception<call_result> result = http_fwd();                                     \
                    if (std::holds_alternative<fc::exception_ptr>(result)) {                                    \
                       try {                                                                                    \
                          throw *std::get<fc::exception_ptr>(result);                                           \
                       } catch (...) {                                                                          \
                          http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                \
                       }                                                                                        \
                    } else {                                                                                    \
                       cb(resp_code, [r = std::get<call_result>(std::move(result))]                             \
                                     (fc::json_writer& w) mutable { fc::to_json_stream(r, w); });               \
                    }                                                                                           \
                 });                                                                                            \
              }                                                                                                 \
           });                                                                                                  \
     } catch (...) {                                                                                            \
        http_plugin::handle_exception_stream(#api_name, #call_name, body, cb);                                  \
     }                                                                                                          \
   }                                                                                                            \
}
