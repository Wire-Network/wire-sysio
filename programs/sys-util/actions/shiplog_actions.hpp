#include "base_actions.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

/// CLI options shared by the ship-log subcommands.
struct shiplog_options {
   std::string state_history_dir = "state-history";
   std::string log_name; ///< --log: a well-known stem (trace_history, ...) or a path to a bundle
   uint32_t    first_block = 0;
   uint32_t    last_block  = std::numeric_limits<uint32_t>::max();
   uint32_t    block_num   = 0; ///< --block: the block a block-id lookup asks about
   std::string output_dir;
   uint32_t    stride = 0;

   // flags
   bool deep      = false;
   bool dry_run   = false;
   bool keep_tail = false;
};

/**
 * sys-util `ship-log` subcommand family: offline inspection and repair of state history (SHiP)
 * log bundles, built on sysio::state_history::log_utils. Mirrors the structure of the `block-log`
 * subcommands.
 */
class shiplog_actions : public base_actions<shiplog_options> {
public:
   shiplog_actions() : base_actions() {}
   void setup(CLI::App& app);

protected:
   /// normalize option paths (relative -> absolute) before any subcommand runs
   void initialize();

   /// resolve --log (required) to a bundle stem; accepts a name in the state-history dir or a path
   std::filesystem::path resolve_stem() const;
   /// resolve --log to one stem, or, when --log was not given, every *.log bundle in the directory
   std::vector<std::filesystem::path> resolve_stems() const;

   int info();
   int block_id();
   int smoke_test();
   int make_index();
   int trim();
   int extract();
   int repair();
   int do_vacuum();
   int split();
   int merge();
};
