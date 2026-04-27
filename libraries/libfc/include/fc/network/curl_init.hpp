// libraries/libfc/include/fc/network/curl_init.hpp
namespace fc {
   // Idempotent, thread-safe one-time init of libcurl's global state.
   // Safe to call from any plugin or thread; actual init happens exactly
   // once per process. Intentionally does not register a cleanup handler --
   // curl's global state is reclaimed at process exit.
   void ensure_libcurl_initialized();
}

// libraries/libfc/src/network/curl_init.cpp
#include <fc/exception/exception.hpp>
#include <curl/curl.h>
#include <mutex>
namespace fc {
   void ensure_libcurl_initialized() {
      static std::once_flag flag;
      std::call_once(flag, []() {
         const auto rc = curl_global_init(CURL_GLOBAL_DEFAULT);
         FC_ASSERT(rc == CURLE_OK, "curl_global_init failed: {}", curl_easy_strerror(rc));
      });
   }
}