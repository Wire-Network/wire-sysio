#pragma once

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <fc/network/http/http_client.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace sysio::outbound_http {

/** Command-line names for one outbound HTTP caller's transport overrides. */
struct transport_option_names {
   const char* additional_ca_file;
   const char* additional_ca_path;
   const char* proxy;
};

/** Process-wide fallback option names shared by every outbound HTTP caller. */
inline constexpr transport_option_names global_transport_option_names{
   .additional_ca_file = "outbound-http-additional-ca-file",
   .additional_ca_path = "outbound-http-additional-ca-path",
   .proxy = "outbound-http-proxy",
};

/** Register process-wide fallback options once through http_client_plugin. */
inline void add_global_transport_program_options(boost::program_options::options_description& options) {
   namespace bpo = boost::program_options;
   options.add_options()(
      global_transport_option_names.additional_ca_file, bpo::value<std::filesystem::path>(),
      "PEM CA bundle added to system trust for all outbound HTTPS callers unless a caller-specific override is set.");
   options.add_options()(
      global_transport_option_names.additional_ca_path, bpo::value<std::filesystem::path>(),
      "Hashed CA directory added to system trust for all outbound HTTPS callers unless a caller-specific override is "
      "set.");
   options.add_options()(
      global_transport_option_names.proxy, bpo::value<std::string>(),
      "Explicit proxy URL used by all outbound HTTP callers unless a caller-specific override is set.");
}

/** Register one caller's overrides; process-wide options have a single owner. */
inline void add_transport_program_options(boost::program_options::options_description& options,
                                          transport_option_names names, std::string_view caller) {
   namespace bpo = boost::program_options;
   const auto scope = std::string(caller);
   options.add_options()(names.additional_ca_file, bpo::value<std::filesystem::path>(),
                         ("PEM CA bundle added to system trust for " + scope + " HTTPS requests.").c_str());
   options.add_options()(names.additional_ca_path, bpo::value<std::filesystem::path>(),
                         ("Hashed CA directory added to system trust for " + scope + " HTTPS requests.").c_str());
   options.add_options()(names.proxy, bpo::value<std::string>(),
                         ("Explicit proxy URL for " + scope + " HTTP requests.").c_str());
}

/** Overlay one caller's local-over-global options onto an existing transport policy. */
inline fc::http::transport_options read_transport_options(const boost::program_options::variables_map& options,
                                                          transport_option_names names,
                                                          fc::http::transport_options result = {}) {
   const auto apply = [&](transport_option_names source) {
      if (options.contains(source.additional_ca_file)) {
         result.additional_ca_file = options.at(source.additional_ca_file).as<std::filesystem::path>();
      }
      if (options.contains(source.additional_ca_path)) {
         result.additional_ca_path = options.at(source.additional_ca_path).as<std::filesystem::path>();
      }
      if (options.contains(source.proxy)) {
         result.proxy = options.at(source.proxy).as<std::string>();
      }
   };
   apply(global_transport_option_names);
   apply(names);
   return result;
}

} // namespace sysio::outbound_http
