// CLAUDE: everything is file is stubbed, it's your job to write the implementation
#include <print>
#include <sysio/chain/application.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/io/json.hpp>
#include <fc/time.hpp>

#include <fc/log/logger_config.hpp>
#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/version/version.hpp>

using namespace appbase;
using namespace sysio;

void configure_logging(const std::filesystem::path& config_path) {
   try {
      try {
         fc::configure_logging(config_path);
      } catch (...) {
         elog("Error reloading logging.json");
         throw;
      }
   } catch (const fc::exception& e) {
      //
      elog("${e}", ("e", e.to_detail_string()));
   } catch (const boost::exception& e) {
      elog("${e}", ("e", boost::diagnostic_information(e)));
   } catch (const std::exception& e) {
      //
      elog("${e}", ("e", e.what()));
   } catch (...) {
      // empty
   }
}

void logging_conf_handler() {
   auto config_path = app().get_logging_conf();
   if (std::filesystem::exists(config_path)) {
      ilog("Received HUP.  Reloading logging configuration from ${p}.", ("p", config_path.string()));
   } else {
      ilog("Received HUP.  No log config found at ${p}, setting to default.", ("p", config_path.string()));
   }
   configure_logging(config_path);
   fc::log_config::initialize_appenders();
}


void initialize_logging() {
   auto config_path = app().get_logging_conf();
   // if (std::filesystem::exists(config_path))
   //    fc::configure_logging(config_path); // intentionally allowing exceptions to escape
   fc::log_config::initialize_appenders();

   app().set_sighup_callback(logging_conf_handler);
}

using namespace fc::network::solana;


struct solana_program_test_counter_data_client : fc::network::solana::solana_program_data_client {
   // CLAUDE: Implement the a client for the counter program referenced in the design doc.
   // solana_program_tx_fn<fc::variant, fc::uint256> set_number;
   // solana_program_call_fn<fc::variant> get_number;
   // solana_program_test_counter_client(const solana_client_ptr& client, const pubkey& program_id,
   //                       const std::vector<idl::program>& idls = {})
   //    : solana_program_client(client, program_id, idls),
   // set_number(create_tx<fc::variant, fc::uint256>(get_idl("setNumber"))),
   // get_number(create_call<fc::variant>(get_idl("number"))) {
   //
   // };
};

struct solana_program_test_counter_anchor_client : fc::network::solana::solana_program_client {
   // CLAUDE: Implement the a client for the counter program referenced in the design doc.
   // solana_program_tx_fn<fc::variant, fc::uint256> set_number;
   // solana_program_call_fn<fc::variant> get_number;
   // solana_program_test_counter_client(const solana_client_ptr& client, const pubkey& program_id,
   //                       const std::vector<idl::program>& idls = {})
   //    : solana_program_client(client, program_id, idls),
   // set_number(create_tx<fc::variant, fc::uint256>(get_idl("setNumber"))),
   // get_number(create_call<fc::variant>(get_idl("number"))) {
   //
   // };
};

/**
 * @brief Main function that demonstrates interaction with the Solana client.
 *
 * This example loads the configuration, creates an Solana client, and performs
 * several Solana RPC method calls to get block numbers, estimate gas, and more.
 *
 *
 * @return int Exit code of the program.
 */
int main(int argc, char* argv[]) {
   // using namespace fc::crypto::ed;
   try {
      appbase::scoped_app app;

      app->set_version_string(sysio::version::version_client());
      app->set_full_version_string(sysio::version::version_full());

      application::register_plugin<signature_provider_manager_plugin>();
      application::register_plugin<outpost_client_plugin>();
      application::register_plugin<outpost_solana_client_plugin>();

      if (!app->initialize<signature_provider_manager_plugin, outpost_client_plugin, outpost_solana_client_plugin>(
         argc, argv, initialize_logging)) {
         const auto& opts = app->get_options();
         if (opts.contains("help") || opts.contains("version") || opts.contains("full-version") ||
             opts.contains("print-default-config")) {
            return 0;
         }
         return 1;
      }

      // auto& sig_plug = app->get_plugin<sysio::signature_provider_manager_plugin>();
      auto& sol_plug = app->get_plugin<sysio::outpost_solana_client_plugin>();


   } catch (const fc::exception& e) {
      elog("${e}", ("e",e.to_detail_string()));
   } catch (const boost::exception& e) {
      elog("${e}", ("e",boost::diagnostic_information(e)));
   } catch (const std::exception& e) {
      elog("${e}", ("e",e.what()));
   } catch (...) {
      elog("unknown exception");
   }
   return 0; ///< Return 0 if the program executed successfully.
}