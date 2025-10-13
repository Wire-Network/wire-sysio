#include "chain.hpp"
#include <memory>
#include <chrono>
#include <format>
#include <filesystem>
#include <pwd.h>

#include <fc/bitutil.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>


#include "config.hpp"
//#include <chainbase/environment.hpp>




#include <boost/exception/diagnostic_information.hpp>
#include <boost/dll.hpp>
#include <boost/process.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <gsl-lite/gsl-lite.hpp>
#include <fc/log/logger_config.hpp>

#include <sysio/chain/block_log.hpp>
#include <sysio/chain/chainbase_environment.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>
#include <sysio/producer_api_plugin/producer_api_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>
#include <sysio/version/version.hpp>
#include <sysio/wallet_plugin/wallet_manager.hpp>


#define ELOG_EXIT(exit_code, fmt, ...) \
do { \
  std::println(std::cerr, fmt, __VA_ARGS__); \
  return exit_code; \
} while(0)

#define ASSERT_ELOG_EXIT(test, fmt, ...) \
if (!test) { \
ELOG_EXIT(-1, fmt, ...) \
} while(0)


using namespace sysio;
using namespace sysio::chain;

namespace {
  namespace fs = std::filesystem;
  FC_DECLARE_EXCEPTION(connection_exception, 1100000, "Connection Exception");

  std::filesystem::path determine_home_directory() {
    std::filesystem::path home;
    passwd* pwd = getpwuid(getuid());
    if (pwd) {
      home = pwd->pw_dir;
    } else {
      home = getenv("HOME");
    }
    if (home.empty()) home = "./";
    return home;
  }

  string default_url = "http://127.0.0.1:8888";
  string default_wallet_url = "unix://" + (determine_home_directory() / "sysio-wallet" / (string(
    sysio::client::config::key_store_executable_name
  ) + ".sock")).string();
  string wallet_url = default_wallet_url; //to be set to default_wallet_url in main


  bool local_port_used() {
    using namespace boost::asio;

    io_service ios;
    local::stream_protocol::endpoint endpoint(wallet_url.substr(strlen("unix://")));
    local::stream_protocol::socket socket(ios);
    boost::system::error_code ec;
    socket.connect(endpoint, ec);

    return !ec;
  }

  void try_local_port(uint32_t duration) {
    using namespace std::chrono;
    auto start_time = duration_cast<std::chrono::milliseconds>(system_clock::now().time_since_epoch()).count();
    while (!local_port_used()) {
      if (duration_cast<std::chrono::milliseconds>(system_clock::now().time_since_epoch()).count() - start_time >
        duration) {
        std::cerr << "Unable to connect to " << client::config::key_store_executable_name << ", if " <<
          client::config::key_store_executable_name << " is running please kill the process and try again.\n";
        throw connection_exception(
          fc::log_messages{
            FC_LOG_MESSAGE(error, "Unable to connect to ${k}", ("k", client::config::key_store_executable_name))
          }
        );
      }
    }
  }


  void find_or_start_kiod(CLI::App* app) {
    if (wallet_url != default_wallet_url) return;

    if (local_port_used()) return;

    std::filesystem::path parent_path{boost::dll::program_location().parent_path().generic_string()};
    std::filesystem::path binPath = parent_path / client::config::key_store_executable_name;
    if (!std::filesystem::exists(binPath)) {
      binPath = parent_path.parent_path() / "kiod" / client::config::key_store_executable_name;
    }

    if (std::filesystem::exists(binPath)) {
      namespace bp = boost::process;
      binPath = std::filesystem::canonical(binPath);

      vector<std::string> pargs;
      pargs.push_back("--http-server-address");
      pargs.push_back("");
      pargs.push_back("--unix-socket-path");
      pargs.push_back(string(client::config::key_store_executable_name) + ".sock");

      ::boost::process::child ksys(
        binPath.string(),
        pargs,
        bp::std_in.close(),
        bp::std_out > bp::null,
        bp::std_err > bp::null
      );
      if (ksys.running()) {
        std::cerr << binPath << " launched" << std::endl;
        ksys.detach();
        try_local_port(2000);
      } else {
        std::cerr << "No wallet service listening on " << wallet_url << ". Failed to launch " << binPath << std::endl;
      }
    } else {
      std::cerr << "No wallet service listening on " << ". Cannot automatically start " <<
        client::config::key_store_executable_name << " because " << client::config::key_store_executable_name <<
        " was not found." << std::endl;
    }
  }

  void log_non_default_options(const std::vector<bpo::basic_option<char>>& options) {
    using namespace std::string_literals;
    string result;
    for (const auto& op : options) {
      bool mask = false;
      if (op.string_key == "signature-provider"s || op.string_key == "peer-private-key"s || op.string_key ==
        "p2p-auto-bp-peer"s) {
        mask = true;
      }
      std::string v;
      for (auto i = op.value.cbegin(), b = op.value.cbegin(), e = op.value.cend(); i != e; ++i) {
        if (i != b) v += ", ";
        if (mask) v += "***";
        else v += *i;
      }

      if (!result.empty()) result += ", ";

      if (v.empty()) {
        result += op.string_key;
      } else {
        result += op.string_key;
        result += " = ";
        result += v;
      }
    }
    ilog("Non-default options: ${v}", ("v", result));
  }


  fc::logging_config& add_deep_mind_logger(fc::logging_config& config) {
    config.appenders.push_back(fc::appender_config("deep-mind", "dmlog"));

    fc::logger_config dmlc;
    dmlc.name = "deep-mind";
    dmlc.level = fc::log_level::debug;
    dmlc.enabled = true;
    dmlc.appenders.push_back("deep-mind");

    config.loggers.push_back(dmlc);

    return config;
  }

  void configure_logging(const std::filesystem::path& config_path) {
    try {
      try {
        if (std::filesystem::exists(config_path)) {
          fc::configure_logging(config_path);
        } else {
          auto cfg = fc::logging_config::default_config();

          fc::configure_logging(add_deep_mind_logger(cfg));
        }
      } catch (...) {
        elog("Error reloading logging.json");
        throw;
      }
    } catch (const fc::exception& e) {
      elog("${e}", ("e",e.to_detail_string()));
    } catch (const boost::exception& e) {
      elog("${e}", ("e",boost::diagnostic_information(e)));
    } catch (const std::exception& e) {
      elog("${e}", ("e",e.what()));
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
    if (std::filesystem::exists(config_path)) fc::configure_logging(config_path);
    // intentionally allowing exceptions to escape
    else {
      auto cfg = fc::logging_config::default_config();

      fc::configure_logging(add_deep_mind_logger(cfg));
    }

    fc::log_config::initialize_appenders();

    app().set_sighup_callback(logging_conf_handler);

  }

} // namespace detail

void chain_actions::setup(CLI::App& app) {
  auto* sub = app.add_subcommand("chain-state", "chain utility");
  sub->add_option(
    "--state-dir",
    opt->sstate_state_dir,
    "The location of the state directory (absolute path or relative to the current directory)"
  )->capture_default_str();
  sub->require_subcommand();
  sub->fallthrough();

  auto* build_info = sub->add_subcommand("build-info", "extract build environment information as JSON");
  build_info->add_option("--output-file,-o", opt->build_output_file, "write into specified file")->
              capture_default_str();
  build_info->add_flag("--print,-p", opt->build_just_print, "print to console");
  build_info->require_option(1);

  build_info->callback(
    [&] {
      int rc = run_subcommand_build_info();
      // properly return err code in main
      if (rc) throw(CLI::RuntimeError(rc));
    }
  );

  sub->add_subcommand("last-shutdown-state", "indicate whether last shutdown was clean or not")->callback(
    [&] {
      int rc = run_subcommand_sstate();
      // properly return err code in main
      if (rc) throw(CLI::RuntimeError(rc));
    }
  );

  // genesis/configure subcommand
  auto* configure = app.add_subcommand("chain-configure", "generate genesis.json in the provided config directory");
  configure->add_option(
    "--target-path,--target,-t",
    opt->configure_target_root,
    "Directory to install config.ini, genesis.json, sysio_key.txt, and wallet"
  )->required();
  configure->add_option("--template", opt->configure_template, "Configuration template to use (allowed: aio)")->
             default_str(chain_configure_template_names[0])->check(CLI::IsMember(chain_configure_template_names));
  configure->add_option(
    "--ini-override-file",
    opt->configure_ini_override_file,
    "Optional path to `config.ini` file to override defaults from /etc/config/nodeop/<template>/config.ini"
  );

  configure->add_option(
    "--genesis-override-file",
    opt->configure_genesis_override_file,
    "Optional path to JSON file to override defaults from /etc/config/nodeop/<template>/genesis.json"
  );
  configure->add_flag(
    "--skip-genesis",
    opt->configure_skip_genesis,
    "Skip generation of genesis.json in the provided config directory"
  );

  configure->add_flag(
    "--skip-ini",
    opt->configure_skip_ini,
    "Skip generation of config.ini in the provided config directory"
  );

  configure->add_flag("--overwrite,-f", opt->configure_overwrite, "Overwrite existing genesis.json if present");
  configure->parse_complete_callback(
    [&app] {
      find_or_start_kiod(&app);
    }
  );
  configure->callback(
    [&] {
      int rc = run_subcommand_configure();
      if (rc) throw(CLI::RuntimeError(rc));
    }
  );
}

int chain_actions::run_subcommand_build_info() {
  if (!opt->build_output_file.empty()) {
    std::filesystem::path p = opt->build_output_file;
    if (p.is_relative()) {
      p = std::filesystem::current_path() / p;
    }
    fc::json::save_to_file(chainbase::environment(), p, true);
    std::cout << "Saved build info JSON to '" << p.generic_string() << "'" << std::endl;
  }
  if (opt->build_just_print) {
    std::cout << fc::json::to_pretty_string(chainbase::environment()) << std::endl;
  }

  return 0;
}

int chain_actions::run_subcommand_sstate() {
  std::filesystem::path state_dir = "";

  // default state dir, if none specified
  if (opt->sstate_state_dir.empty()) {
    auto root = fc::app_path();
    auto default_data_dir = root / "sysio" / "nodeop" / "data";
    state_dir = default_data_dir / config::default_state_dir_name;
  } else {
    // adjust if path relative
    state_dir = opt->sstate_state_dir;
    if (state_dir.is_relative()) {
      state_dir = std::filesystem::current_path() / state_dir;
    }
  }

  auto shared_mem_path = state_dir / "shared_memory.bin";

  if (!std::filesystem::exists(shared_mem_path)) {
    std::cerr << "Unable to read database status: file not found: " << shared_mem_path << std::endl;
    return -1;
  }

  char header[chainbase::header_size];
  std::ifstream hs(shared_mem_path.generic_string(), std::ifstream::binary);
  hs.read(header, chainbase::header_size);
  if (hs.fail()) {
    std::cerr << "Unable to read database status: file invalid or corrupt" << shared_mem_path << std::endl;
    return -1;
  }

  chainbase::db_header* dbheader = reinterpret_cast<chainbase::db_header*>(header);
  if (dbheader->id != chainbase::header_id) {
    std::string what_str(
      "\"" + state_dir.generic_string() + "\" database format not compatible with this version of chainbase."
    );
    std::cerr << what_str << std::endl;
    return -1;
  }
  if (dbheader->dirty) {
    std::cout << "Database dirty flag is set, shutdown was not clean" << std::endl;
    return -1;
  }

  std::cout << "Database state is clean" << std::endl;
  return 0;
}

int chain_actions::run_subcommand_configure() {
  try {
    std::filesystem::path target_dir = std::filesystem::absolute(opt->configure_target_root);

    std::filesystem::path config_dir = target_dir / "config";
    std::filesystem::path secrets_dir = target_dir / "secrets";
    std::filesystem::path wallet_dir = target_dir / "wallet";
    std::filesystem::path data_dir = target_dir / "data";
    auto skip_genesis = opt->configure_skip_genesis;
    auto skip_ini = opt->configure_skip_ini;
    auto overwrite = opt->configure_overwrite;
    std::filesystem::path ini_override_file = opt->configure_ini_override_file;
    std::filesystem::path genesis_override_file = opt->configure_genesis_override_file;

    auto template_name = opt->configure_template;
    if (skip_genesis && skip_ini) throw CLI::RequiredError(
      "Both --skip-genesis & --skip-ini can not be used at the sametime (nothing to do)"
    );
    if (!std::ranges::contains(chain_configure_template_names, template_name)) throw CLI::RequiredError(
      "Invalid template name: " + template_name
    );
    if (target_dir.empty()) throw CLI::RequiredError("No config path specified");

    if (fs::is_directory(data_dir) && overwrite) {
      std::println(std::cout, "Removing existing data directory: {}", data_dir.generic_string());
      fs::remove_all(data_dir);
    }

    // RESOLVE ROOT DIR
    std::filesystem::path root_dir{std::filesystem::current_path()};
    const char* root_path_env = std::getenv("WIRE_ROOT");
    if (root_path_env && std::strlen(root_path_env)) root_dir = root_path_env;

    if (target_dir.generic_string().starts_with(root_dir.generic_string())) {
      std::println(
        std::cerr,
        "Config directory ({}) can not be nested in the root_dir({})",
        config_dir.generic_string(),
        root_dir.generic_string()
      );
      return -1;
    }

    // CREATE PATHS
    for (auto& p : {config_dir, wallet_dir, secrets_dir, data_dir}) {
      if (std::filesystem::exists(p)) continue;
      std::error_code ec;
      std::filesystem::create_directories(p, ec);
      if (ec) {
        std::println(std::cerr, "Failed to create directory '{}': {}", p.generic_string(), ec.message());
        return -1;
      }
    }

    std::println(std::cout, "root_dir: {}", root_dir.generic_string());

    std::filesystem::path template_dir = root_dir / "etc" / "config" / "nodeop" / template_name;
    auto default_genesis_file = template_dir / "genesis.template.json";

    std::println(std::cout, "template_dir: {}", root_dir.generic_string());
    std::println(std::cout, "target_dir: {}", target_dir.generic_string());
    std::println(std::cout, "secrets_dir: {}", secrets_dir.generic_string());
    std::println(std::cout, "config_dir: {}", config_dir.generic_string());

    // Target genesis.json path
    std::filesystem::path target_genesis_file = config_dir / "genesis.json";
    std::filesystem::path target_ini_file = config_dir / "config.ini";
    auto target_genesis_file_exists = std::filesystem::exists(target_genesis_file);
    auto target_ini_file_exists = std::filesystem::exists(target_ini_file);
    auto can_write_genesis = !target_genesis_file_exists || overwrite;
    auto can_write_ini = !target_ini_file_exists || overwrite;

    if (!skip_genesis) {
      if (!can_write_genesis) {
        std::println(
          std::cerr,
          "Refusing to overwrite existing file without --overwrite: {}",
          target_genesis_file.generic_string()
        );
      } else {
        // CREATE NEW WALLET
        auto wallet_file = wallet_dir / "default.wallet";
        if (fs::exists(wallet_file)) {
          std::println(std::cout, "overwriting wallet ({})", wallet_file.generic_string());
          fs::remove(wallet_file);
        }

        std::println(std::cout, "creating wallet ({})", wallet_file.generic_string());
        auto wallet_manager = std::make_unique<sysio::wallet::wallet_manager>();
        wallet_manager->set_dir(wallet_dir);
        auto wallet_pw = wallet_manager->create(wallet_default_name);

        // WALLET & KEY PATHS
        auto wallet_pw_file = secrets_dir / "sysio_wallet_pw.txt";
        auto key_file = secrets_dir / "sysio_key.txt";

        // SAVE WALLET PW
        std::println(std::cout, "saving wallet password to {}", wallet_pw_file.generic_string());
        {
          std::ofstream out(wallet_pw_file.c_str(), std::ofstream::trunc);
          out << wallet_pw;
          out.close();
        }

        // OPEN & UNLOCK WALLET
        std::println(
          std::cout,
          "opening & unlocking wallet {} with password in file {}",
          wallet_file.generic_string(),
          wallet_pw_file.generic_string()
        );
        wallet_manager->open(wallet_default_name);
        wallet_manager->unlock(wallet_default_name, wallet_pw);

        // CREATE KEY
        auto pk = private_key_type::generate();
        auto privs = pk.to_string({});
        auto pubs = pk.get_public_key().to_string({});

        std::println(std::cout, "saving keys to {}", key_file.generic_string());
        {
          std::ofstream out(key_file.c_str());
          out << std::format("Private key: {}\nPublic key: {}\n", privs, pubs);
          out.close();
        }

        // IMPORTING KEY INTO WALLET
        std::println(std::cout, "importing key into wallet {}", wallet_file.generic_string());
        wallet_manager->import_key(wallet_default_name, privs);


        // LOCATE DEFAULT GENESIS TEMPLATE BASED ON SELECTED TEMPLATE
        if (!std::filesystem::exists(default_genesis_file)) {
          std::println(
            std::cerr,
            "Default genesis file not found for template '{}': {}",
            template_name,
            default_genesis_file.generic_string()
          );
          return -1;
        }

        // Load base genesis JSON
        fc::mutable_variant_object base_v{fc::json::from_file(default_genesis_file)};
        fc::mutable_variant_object result_v = base_v;

        // If override provided, shallow-merge top-level keys
        if (!genesis_override_file.empty()) {
          if (!std::filesystem::exists(genesis_override_file)) {
            std::println(std::cerr, "Override file not found: {}", genesis_override_file.generic_string());
            return -1;
          }

          // MERGE OBJECTS
          fc::mutable_variant_object override_v{fc::json::from_file(genesis_override_file)};
          result_v = base_v(override_v);
        }
        auto now = std::chrono::system_clock::now();
        auto epoch_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        auto timestamp = std::format("{:%FT%T}", epoch_ms);
        auto chain_id_data = std::format("wire-{}", timestamp);
        auto chain_id = fc::sha256::hash(chain_id_data).str();
        std::println(std::cout, "chain_id_data: {}, chain_id: {}", chain_id_data, chain_id);
        result_v("initial_timestamp", timestamp);
        result_v("initial_key", pubs);
        result_v("initial_chain_id", chain_id);

        // Write resulting JSON
        std::println(std::cout, "Writing genesis JSON to '{}'", target_genesis_file.generic_string());
        fc::json::save_to_file(result_v, target_genesis_file, true);
        std::println(std::cout, "Saved genesis JSON to '{}'", target_genesis_file.generic_string());


        wallet_manager->lock(wallet_default_name);
      }
    }

    if (!skip_ini) {
      if (!can_write_ini) {
        std::println(
          std::cerr,
          "Refusing to overwrite existing file without --overwrite: {}",
          target_ini_file.generic_string()
        );
      } else {
        auto default_ini_file = template_dir / "config.template.ini";

        if (!std::filesystem::exists(default_ini_file)) {
          std::println(
            std::cerr,
            "Default config.ini file not found for template '{}': {}",
            template_name,
            default_ini_file.generic_string()
          );
          return -1;
        }
        std::println(
          std::cout,
          "Copying default_ini_file({}) to target_ini_file({})",
          default_ini_file.generic_string(),
          target_ini_file.generic_string()
        );
        if (std::filesystem::exists(ini_override_file)) std::println(
          std::cerr,
          "INI OVERRIDE IS NOT IMPLEMENTED YET: {}",
          ini_override_file.generic_string()
        );
        std::filesystem::copy_file(
          default_ini_file,
          target_ini_file,
          std::filesystem::copy_options::overwrite_existing
        );
        std::println(std::cout, "Saved config.ini to '{}'", target_ini_file.generic_string());
      }
    }

    // CONFIG IS COMPLETE, NOW POPULATE THE CHAIN
    std::println(std::cout, "Starting the chain and importing contracts");
    std::atomic_int app_thread_status{-1};
    std::mutex app_thread_mutex;
    std::condition_variable app_thread_cond;
    if (!application::null_app_singleton()) {
      application::instance().shutdown();
      application::reset_app_singleton();
    }
    appbase::scoped_app app;

    auto on_app_init = [&](std::int32_t code) {
      std::scoped_lock lock(app_thread_mutex);
      app_thread_status = code;
      app_thread_cond.notify_all();
    };

    std::thread app_thread(
      [&] {

        auto exe_name = boost::dll::program_location().filename().generic_string();

        fc::scoped_exit<std::function<void()>> on_exit = [&]() {
          ilog(
            "${name} version ${ver} ${fv}",
            ("name", exe_name)("ver", app->version_string()) ("fv", app->version_string() == app->full_version_string()
              ? "" : app->full_version_string())
          );
          log_non_default_options(app->get_parsed_options());
        };

        auto app_cleanup = gsl_lite::finally(
          [&] {
            on_exit.cancel();
            app->shutdown();
          }
        );


        uint32_t short_hash = 0;
        fc::from_hex(sysio::version::version_hash(), reinterpret_cast<char*>(&short_hash), sizeof(short_hash));

        app->set_version(htonl(short_hash));
        app->set_version_string(sysio::version::version_client());
        app->set_full_version_string(sysio::version::version_full());

        auto root = fc::app_path();
        app->set_default_data_dir(data_dir);
        app->set_default_config_dir(config_dir);
        http_plugin::set_defaults(
          {
            .default_unix_socket_path = "",
            .default_http_port = 8888,
            .server_header = exe_name + "/" + app->version_string()
          }
        );

        // int argc = 0;
        // char** argv = nullptr;
        char* argvv[] = {const_cast<char*>(exe_name.c_str()), nullptr};
        if (!app->initialize<chain_plugin, net_plugin, producer_plugin, producer_api_plugin, resource_monitor_plugin>(
          1,
          argvv,
          initialize_logging
        )) {
          on_app_init(1);
          return 1;
        }
        if (auto resmon_plugin = app->find_plugin<resource_monitor_plugin>()) {
          resmon_plugin->monitor_directory(app->data_dir());
        } else {
          elog("resource_monitor_plugin failed to initialize");
          on_app_init(1);
          return 1;
        }
        ilog(
          "${name} version ${ver} ${fv}",
          ("name", exe_name)("ver", app->version_string()) ("fv", app->version_string() == app->full_version_string() ?
            "" : app->full_version_string())
        );
        ilog("${name} using configuration file ${c}", ("name", exe_name)("c", app->full_config_file_path().string()));
        ilog("${name} data directory is ${d}", ("name", exe_name)("d", app->data_dir().string()));
        log_non_default_options(app->get_parsed_options());
        app->startup();
        app->set_thread_priority_max();

        on_app_init(0);
        app->exec();

        return 0;
      }
    );

    //app_thread.join();
    {
      std::unique_lock<std::mutex> lock(app_thread_mutex);
      if (app_thread_status == -1)
        app_thread_cond.wait(lock, [&] { return app_thread_status != -1; });
    }
    if (app_thread_status != 0) {
      std::println(std::cerr, "Error initializing app: {}", app_thread_status.load());
      return -1;
    }

    std::println(std::cout, "Chain initialized, sleep 1s");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::println(std::cout, "Populating contracts");
    // auto& app = application::instance();
    auto chain = app->find_plugin<chain_plugin>();
    ilog("Got chain with id: ${chainId}", ("chainId",chain->get_chain_id()));

    // TODO: Create accounts & load contracts
    app->shutdown();
    app->quit();
    app_thread.join();
    return 0;
  } catch (const std::exception& e) {
    std::println(std::cerr, "Error generating configs: {}", e.what());
    return -1;
  }
}
