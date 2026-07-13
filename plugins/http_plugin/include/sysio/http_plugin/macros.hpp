#pragma once

// Variant-cb async-call macro retained for endpoints that emit a non-JSON content
// type (e.g. prometheus_plugin returning text/plain via fc::variant string payload).
// Streaming JSON endpoints use sysio::bind_stream from <sysio/http_plugin/bind_stream.hpp>.

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
                    std::get<fc::exception_ptr>(result)->rethrow();                                           \
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
                          std::get<fc::exception_ptr>(result)->rethrow();                                     \
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
