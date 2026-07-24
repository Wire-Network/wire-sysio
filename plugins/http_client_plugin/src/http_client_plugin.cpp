#include <sysio/http_client_plugin/http_client_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <fstream>

namespace sysio {

namespace {
constexpr auto option_additional_ca_file = "http-client-additional-ca-file";
constexpr auto option_additional_ca_path = "http-client-additional-ca-path";
constexpr auto option_proxy = "http-client-proxy";
}

http_client_plugin::http_client_plugin():my(new http_client()){}
http_client_plugin::~http_client_plugin(){}

void http_client_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
      (option_additional_ca_file,
       boost::program_options::value<std::filesystem::path>(),
       "PEM CA bundle added to system trust for shared KIOD/signing HTTPS requests.")
      (option_additional_ca_path,
       boost::program_options::value<std::filesystem::path>(),
       "Hashed CA directory added to system trust for shared KIOD/signing HTTPS requests.")
      (option_proxy,
       boost::program_options::value<std::string>(),
       "Explicit proxy URL for shared KIOD/signing HTTP requests.");
}

void http_client_plugin::plugin_initialize(const variables_map& options) {
   fc::http::transport_options transport_options;
   if (options.contains(option_additional_ca_file))
      transport_options.additional_ca_file =
         options.at(option_additional_ca_file).as<std::filesystem::path>();
   if (options.contains(option_additional_ca_path))
      transport_options.additional_ca_path =
         options.at(option_additional_ca_path).as<std::filesystem::path>();
   if (options.contains(option_proxy))
      transport_options.proxy = options.at(option_proxy).as<std::string>();
   my = std::make_unique<fc::http_client>(std::move(transport_options));
}

void http_client_plugin::plugin_startup() {

}

void http_client_plugin::plugin_shutdown() {

}

}
