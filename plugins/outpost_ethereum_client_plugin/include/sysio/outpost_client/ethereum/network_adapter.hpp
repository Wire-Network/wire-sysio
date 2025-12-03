// SPDX-License-Identifier: MIT
#pragma once

#include <curl/curl.h>
#include <optional>
#include <string>
#include <fc-lite/macros.hpp>

namespace sysio::outpost_client::ethereum {

/**
 * @class network_adapter
 * @brief A class that handles HTTP requests to an Ethereum node using libcurl.
 *
 * This class provides methods to send HTTP POST requests to an Ethereum node,
 * retrieve the response, and handle the low-level network communication using libcurl.
 * It is designed to be used by other classes to interact with an Ethereum node.
 */
class LIB_EXPORT network_adapter {
public:
   /**
    * @brief Constructs a NetworkAdapter instance.
    *
    * Initializes the libcurl library and prepares the curl handle for making requests.
    */
   network_adapter();

   /**
    * @brief Destructs the NetworkAdapter instance.
    *
    * Cleans up the curl handle and shuts down the libcurl library.
    */
   ~network_adapter();

   /**
    * @brief Sends a POST request to the specified URL with the provided data.
    * @param url The URL to send the POST request to (e.g., the Ethereum node endpoint).
    * @param data The data to send in the body of the POST request (usually a JSON-RPC request).
    * @return The response body as a string if successful, or an empty std::optional if an error occurs.
    */
   std::optional<std::string> send_post_request(const std::string& url, const std::string& data);

private:
   CURL* curl_;
   char  error_buffer_[CURL_ERROR_SIZE];

   /**
    * @brief Callback function used by libcurl to write the response data.
    * @param ptr The received data chunk.
    * @param size The size of each chunk of data.
    * @param chunk_count The number of chunks.
    * @param userdata A pointer to the user data (used to store the response).
    * @return The number of bytes successfully written.
    */
   static size_t write_callback(char* ptr, size_t size, size_t chunk_count, void* userdata);
};

} // namespace sysio::outpost_client::ethereum
