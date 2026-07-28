#include <sysio/http_client_plugin/http_client_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/http_client_plugin/http_client_options.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <fstream>

namespace sysio {

namespace {
constexpr outbound_http::transport_option_names
   transport_option_names{
      .additional_ca_file =
         "http-client-additional-ca-file",
      .additional_ca_path =
         "http-client-additional-ca-path",
      .proxy = "http-client-proxy",
   };
}

http_client_plugin::http_client_plugin():my(new http_client()){}
http_client_plugin::~http_client_plugin(){}

void http_client_plugin::set_program_options(options_description&, options_description& cfg) {
   outbound_http::add_global_transport_program_options(cfg);
   outbound_http::add_transport_program_options(
      cfg,
      transport_option_names,
      "shared KIOD/signing");
}

void http_client_plugin::plugin_initialize(const variables_map& options) {
   my->set_transport_options(
      outbound_http::read_transport_options(
         options,
         transport_option_names));
}

void http_client_plugin::plugin_startup() {

}

void http_client_plugin::plugin_shutdown() {

}

}
