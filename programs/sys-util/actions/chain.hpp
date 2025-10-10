#pragma once
#include "subcommand.hpp"

constexpr std::array<std::string,1> chain_configure_template_names = {"aio"};
constexpr std::string wallet_default_name = "default";

struct chain_options {
  bool build_just_print = false;
  std::string build_output_file = "";
  std::string sstate_state_dir = "";

  // Configure options
  std::string configure_target_root = ""; // directory to write resulting data
  bool configure_overwrite = false; // force overwrite existing files
  bool configure_skip_genesis = false; // skip writing genesis.json and/or creating key pairs
  bool configure_skip_ini = false; // skip writing config.ini
  std::string configure_template = chain_configure_template_names[0];
  std::string configure_genesis_override_file = ""; // optional override JSON file path
  std::string configure_ini_override_file = ""; // optional override JSON file path
};

class chain_actions : public sub_command<chain_options> {
  public:

    chain_actions() : sub_command() {
    }

    void setup(CLI::App& app);

    // callbacks
    int run_subcommand_build_info();

    int run_subcommand_sstate();

    int run_subcommand_configure();
};
