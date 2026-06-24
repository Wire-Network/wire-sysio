#include "chain_actions.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <fc/io/json.hpp>
#include <chainbase/environment.hpp>
#include <sysio/chain/app.hpp>
#include <sysio/chain/config.hpp>

using namespace sysio::chain;

namespace {
  namespace fs = std::filesystem;
} // namespace

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
      // properly return err code in main
      if (int rc = run_subcommand_build_info();rc) throw(CLI::RuntimeError(rc));
    }
  );

  sub->add_subcommand("last-shutdown-state", "indicate whether last shutdown was clean or not")->callback(
    [&] {
      int rc = run_subcommand_sstate();
      // properly return err code in main
      if (rc) throw(CLI::RuntimeError(rc));
    }
  );
}

int chain_actions::run_subcommand_build_info() {
  if (!opt->build_output_file.empty()) {
    fs::path p = opt->build_output_file;
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
  fs::path state_dir = "";

  // default state dir, if none specified
  if (opt->sstate_state_dir.empty()) {
    state_dir = default_data_path() / config::default_state_dir_name;
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

  auto dbheader = reinterpret_cast<chainbase::db_header*>(header);
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
