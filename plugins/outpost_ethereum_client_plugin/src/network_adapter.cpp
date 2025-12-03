#include <sysio/outpost_client/ethereum/network_adapter.hpp>

#include <cstring>   // std::memset
#include <utility>   // std::move

namespace sysio::outpost_client::ethereum {

network_adapter::network_adapter()
    : curl_(nullptr)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();

    error_buffer_[0] = '\0';

    if (curl_) {
        // auto* vi = curl_version_info(CURLVERSION_NOW);
        // if (vi && vi->ssl_version) {
        //     Logger::getInstance().log(std::string("curl SSL backend: ") + vi->ssl_version);
        // } else {
        //     Logger::getInstance().log("curl SSL backend: <none>");
        // }

        curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error_buffer_);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    } else {
        // Logger::getInstance().log("Failed to initialize CURL handle");
    }
}

network_adapter::~network_adapter() {
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
    curl_global_cleanup();
}

size_t network_adapter::write_callback(char* ptr, size_t size, size_t chunk_count, void* userdata) {
    const size_t realSize = size * chunk_count;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, realSize);
    return realSize;
}

std::optional<std::string> network_adapter::send_post_request(const std::string& url,
                                                           const std::string& jsonBody) {
    if (!curl_) {
        // Logger::getInstance().log("CURL handle not initialized");
        return std::nullopt;
    }

    // Reset all per-request options to a known state
    curl_easy_reset(curl_);

    // Re-attach error buffer after reset
    error_buffer_[0] = '\0';
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error_buffer_);

    std::string response;
    struct curl_slist* headers = nullptr;

    // --- Headers ---
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) {
        // Logger::getInstance().log("Failed to allocate CURL headers");
        return std::nullopt;
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    // --- Basic request setup ---
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);

    // IMPORTANT: libcurl copies this buffer when using CURLOPT_POSTFIELDS
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(
        curl_,
        CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(jsonBody.size())
    );

    // --- Response handling ---
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &network_adapter::write_callback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    // --- Reasonable timeouts ---
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);

    // If you need strict SSL verification, keep this as 1L (default).
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

    // --- Perform request ---
    CURLcode res = curl_easy_perform(curl_);

    // Always free headers
    curl_slist_free_all(headers);
    headers = nullptr;

    if (res != CURLE_OK) {
        std::string msg = "CURL error: ";
        msg += curl_easy_strerror(res);

        if (error_buffer_[0] != '\0') {
            msg += " | ";
            msg += error_buffer_;
        }

        // Logger::getInstance().log(msg);
        return std::nullopt;
    }

    return response;
}

} // namespace sysio::outpost_client::ethereum