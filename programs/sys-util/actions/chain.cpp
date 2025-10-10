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

#include <sysio/chain/block_log.hpp>
#include <sysio/chain/chainbase_environment.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/version/version.hpp>
#include <sysio/wallet_plugin/wallet_manager.hpp>

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

}

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
        std::println(
          std::cout,
          "importing key into wallet {}",
          wallet_file.generic_string()
        );
        wallet_manager->import_key(wallet_default_name, privs);
        wallet_manager->lock(wallet_default_name);

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
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error generating genesis.json: " << e.what() << std::endl;
    return -1;
  }
}
