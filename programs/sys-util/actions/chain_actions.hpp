#pragma once
#include "base_actions.hpp"

/**
 * Options backing the `chain-state` subcommand group.
 */
struct chain_options {
  bool build_just_print = false;      ///< `build-info --print`: dump build environment JSON to stdout
  std::string build_output_file = ""; ///< `build-info --output-file`: write build environment JSON here
  std::string sstate_state_dir = "";  ///< `--state-dir` override for `last-shutdown-state`
};

/**
 * sys-util `chain-state` diagnostics: build environment information and
 * last-shutdown (clean/dirty) database state detection.
 */
class chain_actions : public base_actions<chain_options> {
  public:

    chain_actions() : base_actions() {
    }

    /// Register the `chain-state` subcommand tree on @p app.
    void setup(CLI::App& app);

    /// `chain-state build-info`: emit the build environment as JSON. Returns a process exit code.
    int run_subcommand_build_info();

    /// `chain-state last-shutdown-state`: report whether the last shutdown was clean. Returns a process exit code.
    int run_subcommand_sstate();
};
