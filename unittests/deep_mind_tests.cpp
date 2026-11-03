#include <sysio/testing/tester.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/io/cfile.hpp>
#include <sysio/chain/deep_mind.hpp>

#include <boost/test/unit_test.hpp>

#include <deep-mind.hpp>

#include "kv_write_driver.hpp"


using namespace sysio::testing;

extern void setup_test_logging();

struct deep_mind_log_fixture
{
   fc::temp_cfile tmp;
   deep_mind_handler deep_mind_logger;

   deep_mind_log_fixture()
   {
      auto cfg = fc::logging_config::default_config();
      tmp.file().close();
      cfg.sinks.push_back(sink_config("dmlog", "dmlog_sink",
                                      mutable_variant_object()("file", tmp.file().get_file_path().c_str())));

      fc::logger_config lc;
      lc.name = "dmlog";
      lc.level = fc::log_level::all;
      lc.sinks.push_back("dmlog");
      cfg.loggers.push_back( lc );

      fc::configure_logging(cfg);
      setup_test_logging();

      deep_mind_logger.update_config(deep_mind_handler::deep_mind_config{.zero_elapsed = true});
      deep_mind_logger.update_logger("dmlog");
   }
   ~deep_mind_log_fixture()
   {
      fc::configure_logging(fc::logging_config::default_config());
      setup_test_logging();
   }
};

// We only test deep-mind in Savanna
struct deep_mind_tester : deep_mind_log_fixture, savanna_validating_tester
{
   // do not load roa, so test does not have to be updated everytime roa changes
   deep_mind_tester() : savanna_validating_tester({}, &deep_mind_logger, setup_policy::preactivate_feature_only) {
      set_bios_contract();
      produce_block();
   }
};

namespace {

void compare_files(const std::string& filename1, const std::string& filename2)
{
   std::ifstream file1(filename1.c_str());
   std::ifstream file2(filename2.c_str());
   std::string line1, line2;
   int i = 1;
   while(std::getline(file2, line2))
   {
      if(!std::getline(file1, line1))
      {
         BOOST_TEST(false, "Unexpected end of input of file1 at line " << i);
         return;
      }
      if(line1 != line2)
      {
         BOOST_TEST(false, "Mismatch at line " << i << "\n+ " << line1 << "\n- " << line2);
         return;
      }
      ++i;
   }
   if(std::getline(file1, line1))
   {
      BOOST_TEST(false, "Expected end of file of file1 at line " << i);
   }
}

/// Every line of the log at `path`, without the DMLOG prefix.
std::vector<std::string> dmlog_lines(const std::filesystem::path& path) {
   const std::string prefix = "DMLOG ";
   std::vector<std::string> lines;
   std::ifstream log(path);
   for (std::string line; std::getline(log, line);) {
      if (line.starts_with(prefix)) lines.push_back(line.substr(prefix.size()));
   }
   return lines;
}

/// The lines of the log at `path` that name `account` and report a KV write or a RAM charge, without the DMLOG prefix.
std::vector<std::string> kv_and_ram_lines(const std::filesystem::path& path, account_name account) {
   const std::vector<std::string> ops = {"KV_OP ", "KV_IDX_OP ", "RAM_OP "};
   std::vector<std::string> lines;
   for (const auto& line : dmlog_lines(path)) {
      const bool reports_a_write =
         std::ranges::any_of(ops, [&](const std::string& op) { return line.starts_with(op); });
      if (reports_a_write && line.find(account.to_string()) != std::string::npos) lines.push_back(line);
   }
   return lines;
}

/// The trace an APPLIED_TRANSACTION line carries.
transaction_trace unpack_applied_transaction(const std::string& line) {
   const auto hex = line.substr(line.rfind(' ') + 1);
   std::vector<char> packed(hex.size() / 2);
   fc::from_hex(hex, packed.data(), packed.size());
   return fc::raw::unpack<transaction_trace>(packed);
}

} // namespace

/// A chain with the KV write driver on `writer`, and the RAM lines that come next easy to spell.
///
/// The base fixture logs from the validating node, which sees only blocks, so a transaction that aborts or a dry run never
/// reaches it. These tests watch the node that runs the transaction, and read the log before a block is produced, which
/// would log the same lines again from the validating node.
struct deep_mind_kv_tester : deep_mind_tester {
   static constexpr uint16_t table_id = 7;
   const account_name writer      = "kvwriter"_n;
   const account_name other_payer = "alice"_n;

   deep_mind_kv_tester() {
      create_account(writer);
      create_account(other_payer);
      set_code(writer, kv_write_driver::kv_write_wast);
      produce_block();
      control->enable_deep_mind(&deep_mind_logger);
   }

   int64_t ram(account_name account) const {
      return control->get_resource_limits_manager().get_account_ram_usage(account);
   }

   std::filesystem::path log() { return tmp.file().get_file_path(); }

   /// The KV and RAM lines from `first` on.
   std::vector<std::string> lines_since(size_t first) {
      const auto lines = kv_and_ram_lines(log(), writer);
      return {lines.begin() + first, lines.end()};
   }
};

BOOST_AUTO_TEST_SUITE(deep_mind_tests)

BOOST_FIXTURE_TEST_CASE(deep_mind, deep_mind_tester)
{
   // We have already transitioned into Savanna
   create_account( "alice"_n );
   push_action(config::system_account_name, "updateauth"_n, "alice"_n, fc::mutable_variant_object()
               ("account", "alice")
               ("permission", "test1")
               ("parent", "active")
               ("auth", authority{{"sysio"_n, "active"_n}}));
   produce_block();

   // Update proposer schedule
   vector<account_name> producers = { "bob"_n, "carol"_n, "charlie"_n };
   create_accounts(producers);
   set_producers(producers);

   // Produce 2 rounds to make the schedule active
   produce_blocks(config::producer_repetitions * 2);

   // One KV write of each kind, each in its own action.
   using namespace kv_write_driver;
   constexpr uint16_t table_id = 7;
   const account_name writer = "kvwriter"_n;
   const account_name other_payer = "alice"_n;
   create_account(writer);
   set_code(writer, kv_write_wast);
   produce_block();
   const auto setup_lines = kv_and_ram_lines(tmp.file().get_file_path(), writer).size();

   push_kv_writes(*this, writer, {
      set_row(table_id, writer, "row", "v1", kv_set_mode::insert),
      expecting(kv_status::exists, set_row(table_id, writer, "row", "v9", kv_set_mode::insert)),
      set_row(table_id, writer, "row", "v2", kv_set_mode::update),
      set_row(table_id, writer, "row", "v3v3", kv_set_mode::update),
      set_row(table_id, other_payer, "row", "v3v3", kv_set_mode::update),
      erase_row(table_id, "row"),
      store_entry(table_id, writer, "pk", "s1"),
      expecting(kv_status::exists, store_entry(table_id, writer, "pk", "s1")),
      update_entry(table_id, writer, "pk", "s1", "s2"),
      update_entry(table_id, writer, "pk", "s2", "s3s3"),
      update_entry(table_id, other_payer, "pk", "s3s3", "s3s3"),
      erase_entry(table_id, "pk", "s3s3"),
   });
   produce_block();

   // The golden log is regenerated whole, so what it has to pin is spelled out here. "row" is 726f77, "pk" is 706b,
   // "v1" is 7631, "s1" is 7331. A same-size update charges nothing, so it has no RAM_OP line, and a payer change has
   // two. Actions 1 and 7 are refused writes, which log nothing.
   const std::vector<std::string> expected = {
      "KV_OP INS 0 kvwriter kvwriter 7 726f77 7631",
      "RAM_OP 0 kvwriter:7:726f77 table_row add primary_index_add kvwriter 11377 117",
      "KV_OP UPD 2 kvwriter:kvwriter kvwriter 7 726f77 7631:7632",
      "KV_OP UPD 3 kvwriter:kvwriter kvwriter 7 726f77 7632:76337633",
      "RAM_OP 3 kvwriter:7:726f77 table_row update primary_index_update kvwriter 11379 2",
      "KV_OP UPD 4 kvwriter:alice kvwriter 7 726f77 76337633:76337633",
      "RAM_OP 4 kvwriter:7:726f77 table_row remove primary_index_update_remove_old_payer kvwriter 11260 -119",
      "RAM_OP 4 kvwriter:7:726f77 table_row add primary_index_update_add_new_payer alice 1383 119",
      "KV_OP REM 5 alice kvwriter 7 726f77 76337633",
      "RAM_OP 5 kvwriter:7:726f77 table_row remove primary_index_remove alice 1264 -119",
      "KV_IDX_OP INS 6 kvwriter kvwriter 7 706b 7331",
      "RAM_OP 6 kvwriter:7:706b:7331 secondary_index add secondary_index_add kvwriter 11376 116",
      "KV_IDX_OP UPD 8 kvwriter:kvwriter kvwriter 7 706b 7331:7332",
      "KV_IDX_OP UPD 9 kvwriter:kvwriter kvwriter 7 706b 7332:73337333",
      "RAM_OP 9 kvwriter:7:706b:73337333 secondary_index update secondary_index_update kvwriter 11378 2",
      "KV_IDX_OP UPD 10 kvwriter:alice kvwriter 7 706b 73337333:73337333",
      "RAM_OP 10 kvwriter:7:706b:73337333 secondary_index remove secondary_index_update_remove_old_payer "
      "kvwriter 11260 -118",
      "RAM_OP 10 kvwriter:7:706b:73337333 secondary_index add secondary_index_update_add_new_payer alice 1382 118",
      "KV_IDX_OP REM 11 alice kvwriter 7 706b 73337333",
      "RAM_OP 11 kvwriter:7:706b:73337333 secondary_index remove secondary_index_remove alice 1264 -118",
   };
   const auto lines = kv_and_ram_lines(tmp.file().get_file_path(), writer);
   BOOST_TEST(std::vector<std::string>(lines.begin() + setup_lines, lines.end()) == expected,
              boost::test_tools::per_element());

   bool save_log = [](){
      auto argc = boost::unit_test::framework::master_test_suite().argc;
      auto argv = boost::unit_test::framework::master_test_suite().argv;
      return std::find(argv, argv + argc, std::string("--save-dmlog")) != (argv + argc);
   }();

   auto log_output_path = tmp.file().get_file_path();

   if(save_log)
   {
      std::filesystem::copy(log_output_path, DEEP_MIND_LOGFILE, std::filesystem::copy_options::update_existing);
   }
   else
   {
      compare_files(log_output_path.string(), DEEP_MIND_LOGFILE);
   }
}

// An abort takes back no line. What an aborted transaction wrote stays in the stream, every action of it, and the
// APPLIED_TRANSACTION line after them is what tells a consumer to drop them: its trace carries `except`.
BOOST_FIXTURE_TEST_CASE(kv_lines_of_an_aborted_transaction_stay, deep_mind_kv_tester)
{
   using namespace kv_write_driver;
   const auto first_line = kv_and_ram_lines(log(), writer).size();
   const auto all_lines  = dmlog_lines(log()).size();
   const auto usage      = ram(writer);

   BOOST_CHECK_THROW(push_kv_writes(*this, writer, {
      set_row(table_id, writer, "row", "v1"),
      store_entry(table_id, writer, "pk", "s1"),
      expecting(kv_status::exists, set_row(table_id, writer, "row", "v2", kv_set_mode::update)),
   }), sysio_assert_message_exception);

   const std::vector<std::string> expected = {
      "KV_OP INS 0 kvwriter kvwriter 7 726f77 7631",
      "RAM_OP 0 kvwriter:7:726f77 table_row add primary_index_add kvwriter " + std::to_string(usage + 117) + " 117",
      "KV_IDX_OP INS 1 kvwriter kvwriter 7 706b 7331",
      "RAM_OP 1 kvwriter:7:706b:7331 secondary_index add secondary_index_add kvwriter " +
         std::to_string(usage + 117 + 116) + " 116",
      "KV_OP UPD 2 kvwriter:kvwriter kvwriter 7 726f77 7631:7632",
   };
   BOOST_TEST(lines_since(first_line) == expected, boost::test_tools::per_element());

   // The RAM those lines report did not stay.
   BOOST_CHECK_EQUAL(ram(writer), usage);

   const auto lines = dmlog_lines(log());
   std::vector<std::string> applied;
   for (size_t i = all_lines; i < lines.size(); ++i) {
      if (lines[i].starts_with("APPLIED_TRANSACTION ")) applied.push_back(lines[i]);
   }
   BOOST_REQUIRE_EQUAL(applied.size(), 1u);
   BOOST_TEST(lines.back().starts_with("APPLIED_TRANSACTION "));

   const auto trace = unpack_applied_transaction(applied[0]);
   BOOST_REQUIRE(trace.except);
   BOOST_TEST(trace.except->to_detail_string().find("unexpected kv status") != std::string::npos);
   BOOST_CHECK(!trace.receipt.has_value());
}

// A dry run makes its writes, and undoes them, without a line in the stream.
BOOST_FIXTURE_TEST_CASE(kv_dry_run_writes_log_nothing, deep_mind_kv_tester)
{
   using namespace kv_write_driver;
   const auto first_line = kv_and_ram_lines(log(), writer).size();
   const auto usage      = ram(writer);

   auto trx = kv_writes_trx(*this, writer, {set_row(table_id, writer, "row", "v1"),
                                            store_entry(table_id, writer, "pk", "s1"),
                                            set_row(table_id, writer, "row", "v2", kv_set_mode::update),
                                            update_entry(table_id, writer, "pk", "s1", "s2"),
                                            erase_row(table_id, "row"),
                                            set_row(table_id, writer, "left", "behind")});
   const auto trace = push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US, false,
                                       transaction_metadata::trx_type::dry_run);
   BOOST_REQUIRE(!trace->except);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 6u);

   BOOST_TEST(lines_since(first_line).empty());
   BOOST_CHECK_EQUAL(ram(writer), usage);
}

// A re-key and a payer change in one call. The event id names the new key on both RAM lines, and the old key appears only
// on the KV_IDX_OP line. An empty value leaves its field empty: INS and REM end in a space, and UPD has nothing on that
// side of the colon.
BOOST_FIXTURE_TEST_CASE(kv_lines_for_a_rekey_with_a_payer_change_and_an_empty_value, deep_mind_kv_tester)
{
   using namespace kv_write_driver;
   const auto first_line = kv_and_ram_lines(log(), writer).size();
   const auto writer_ram = ram(writer);
   const auto payer_ram  = ram(other_payer);

   push_kv_writes(*this, writer, {
      store_entry(table_id, writer, "pk", "s1"),
      update_entry(table_id, other_payer, "pk", "s1", "s2s2"),
      set_row(table_id, writer, "row", ""),
      set_row(table_id, writer, "row", "v1", kv_set_mode::update),
      set_row(table_id, writer, "row", "", kv_set_mode::update),
      erase_row(table_id, "row"),
   });

   const auto at = [&](int64_t delta) { return std::to_string(writer_ram + delta); };
   const std::vector<std::string> expected = {
      "KV_IDX_OP INS 0 kvwriter kvwriter 7 706b 7331",
      "RAM_OP 0 kvwriter:7:706b:7331 secondary_index add secondary_index_add kvwriter " + at(116) + " 116",
      "KV_IDX_OP UPD 1 kvwriter:alice kvwriter 7 706b 7331:73327332",
      "RAM_OP 1 kvwriter:7:706b:73327332 secondary_index remove secondary_index_update_remove_old_payer kvwriter " +
         at(0) + " -116",
      "RAM_OP 1 kvwriter:7:706b:73327332 secondary_index add secondary_index_update_add_new_payer alice " +
         std::to_string(payer_ram + 118) + " 118",
      "KV_OP INS 2 kvwriter kvwriter 7 726f77 ",
      "RAM_OP 2 kvwriter:7:726f77 table_row add primary_index_add kvwriter " + at(115) + " 115",
      "KV_OP UPD 3 kvwriter:kvwriter kvwriter 7 726f77 :7631",
      "RAM_OP 3 kvwriter:7:726f77 table_row update primary_index_update kvwriter " + at(117) + " 2",
      "KV_OP UPD 4 kvwriter:kvwriter kvwriter 7 726f77 7631:",
      "RAM_OP 4 kvwriter:7:726f77 table_row update primary_index_update kvwriter " + at(115) + " -2",
      "KV_OP REM 5 kvwriter kvwriter 7 726f77 ",
      "RAM_OP 5 kvwriter:7:726f77 table_row remove primary_index_remove kvwriter " + at(0) + " -115",
   };
   BOOST_TEST(lines_since(first_line) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()
