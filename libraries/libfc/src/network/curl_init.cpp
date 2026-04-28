// libraries/libfc/src/network/curl_init.cpp

#include <fc/network/curl_init.hpp>
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