#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/program_options.hpp>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/app.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/contract_root_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/http_client_plugin/http_client_options.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>
#include <sysio/testing/tester.hpp>
#include <snapshot_attest_fixture.hpp>
#include <stdint.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace snapshot_attest = sysio::protocol::snapshot_attestation;
namespace http = boost::beast::http;

/** Shared command-line and fixture constants for chain-plugin configuration tests. */
constexpr auto chain_plugin_test_program_name = "test_chain_plugin";
constexpr auto snapshot_option_name = "--snapshot";
constexpr auto snapshot_endpoint_option_name = "--snapshot-endpoint";
constexpr auto blocks_dir_option_name = "--blocks-dir";
constexpr auto config_dir_option_name = "--config-dir";
constexpr auto data_dir_option_name = "--data-dir";
constexpr auto snapshot_without_attestation_table_filename =
   "snapshot-without-attestation-table.bin";
constexpr auto valid_snapshot_filename = "snapshot-with-valid-attestation.bin";
constexpr auto unconfigured_snapshot_filename = "snapshot-without-enabled-attestation.bin";
constexpr auto snapshot_metadata_path = "/v1/snapshot/latest";
constexpr auto snapshot_by_block_path = "/v1/snapshot/by_block";
constexpr auto snapshot_download_path = "/v1/snapshot/download";
constexpr auto snapshot_root_hash_field = "root_hash";
constexpr auto json_content_type = "application/json";
constexpr auto binary_content_type = "application/octet-stream";
constexpr auto loopback_address = "127.0.0.1";
constexpr auto loopback_url_prefix = "http://127.0.0.1:";
constexpr auto url_path_separator = "/";
constexpr auto incompatible_index_type = "i128";
constexpr auto incompatible_record_type = "other_record";
constexpr auto loaded_block_seed = "loaded block";
constexpr auto other_block_seed = "other block";
constexpr auto invalid_block_segment = "not-a-block";
constexpr auto loaded_snapshot_hash =
   "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr auto other_snapshot_hash =
   "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100";
constexpr auto unconfigured_attestation_error_fragment = "min_providers to be configured before bootstrap";
constexpr uint16_t incompatible_table_id = 0;
constexpr uint32_t snapshot_source_block_count = 3;
constexpr uint32_t attestation_finality_block_count = 24;
constexpr uint32_t snapshot_endpoint_response_count = 2;
constexpr uint16_t ephemeral_port = 0;

/** Serve one snapshot endpoint metadata response and its binary download on loopback. */
class snapshot_endpoint_server {
public:
   /** Start a bounded two-request server for the supplied scheduled snapshot. */
   snapshot_endpoint_server(uint32_t block_num, const fc::crypto::blake3& root_hash,
                            const std::filesystem::path& snapshot_path)
      : metadata_body(fc::json::to_string(
           fc::mutable_variant_object()(snapshot_attest::field::block_num, block_num)
              (snapshot_root_hash_field, root_hash.str()),
           fc::time_point::maximum()))
      , snapshot_body(read_snapshot(snapshot_path))
      , acceptor(io, tcp::endpoint(boost::asio::ip::make_address(loopback_address), ephemeral_port))
      , port(acceptor.local_endpoint().port())
      , worker([this] { serve(); }) {}

   snapshot_endpoint_server(const snapshot_endpoint_server&) = delete;
   snapshot_endpoint_server& operator=(const snapshot_endpoint_server&) = delete;

   /** Stop an unused accept and join the server thread. */
   ~snapshot_endpoint_server() {
      // Wake every accept the bounded worker could still be waiting on. If it already returned,
      // the extra loopback connects are harmlessly queued until the acceptor closes.
      for (uint32_t response_index = 0; response_index < snapshot_endpoint_response_count;
           ++response_index) {
         boost::system::error_code error;
         boost::asio::io_context connector_io;
         tcp::socket socket(connector_io);
         socket.connect(tcp::endpoint(boost::asio::ip::make_address(loopback_address), port), error);
         socket.close(error);
      }
      if (worker.joinable()) {
         worker.join();
      }
      boost::system::error_code error;
      acceptor.close(error);
   }

   /** Return the loopback base URL selected for the server. */
   std::string url() const {
      return loopback_url_prefix + std::to_string(port);
   }

private:
   using tcp = boost::asio::ip::tcp;

   /** Read a snapshot file without altering its binary bytes. */
   static std::string read_snapshot(const std::filesystem::path& path) {
      std::ifstream input(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
   }

   /** Serve the metadata request followed by the snapshot download request. */
   void serve() {
      for (uint32_t response_index = 0; response_index < snapshot_endpoint_response_count;
           ++response_index) {
         boost::system::error_code error;
         tcp::socket socket(io);
         acceptor.accept(socket, error);
         if (error) {
            return;
         }

         boost::beast::flat_buffer request_buffer;
         http::request<http::string_body> request;
         http::read(socket, request_buffer, request, error);
         if (error) {
            return;
         }

         const std::string target(request.target().data(), request.target().size());
         const bool is_metadata = target == snapshot_metadata_path || target == snapshot_by_block_path;
         const bool is_download = target == snapshot_download_path;
         http::response<http::string_body> response{
            is_metadata || is_download ? http::status::ok : http::status::not_found,
            request.version()};
         response.set(http::field::content_type,
                      is_metadata ? json_content_type : binary_content_type);
         response.keep_alive(false);
         if (is_metadata) {
            response.body() = metadata_body;
         } else if (is_download) {
            response.body() = snapshot_body;
         }
         response.prepare_payload();
         http::write(socket, response, error);
      }
   }

   /// Serialized response for `/v1/snapshot/latest`.
   std::string metadata_body;
   /// Exact snapshot bytes returned by `/v1/snapshot/download`.
   std::string snapshot_body;
   /// Event loop owned by the server thread.
   boost::asio::io_context io;
   /// Loopback listener retained until teardown.
   tcp::acceptor acceptor;
   /// Ephemeral port assigned by the operating system.
   uint16_t port;
   /// Worker serving the bounded request sequence.
   std::thread worker;
};

/** Write a production-shaped snapshot and return its deterministic root hash. */
fc::crypto::blake3 write_chain_plugin_snapshot(
   sysio::chain::controller& control, const std::filesystem::path& snapshot_path) {
   auto writer = std::make_shared<sysio::chain::threaded_snapshot_writer>(snapshot_path);
   control.write_snapshot(writer);
   // chain_plugin enables root-extension tracking while the tester fixture does not.
   writer->write_section<sysio::chain::contract_root_object>([](auto&) {});
   writer->finalize();
   return writer->get_root_hash();
}

/** Build the exact attestation ABI fragment required before auto-fetched snapshot replay. */
sysio::chain::abi_def make_snapshot_attestation_abi() {
   sysio::chain::abi_def abi;
   abi.tables.emplace_back(
      snapshot_attest::table_snaprecords,
      snapshot_attest::index_type_i64,
      std::vector<sysio::chain::field_name>{snapshot_attest::field::block_num},
      std::vector<sysio::chain::type_name>{snapshot_attest::abi_type::uint64},
      snapshot_attest::type_snap_record,
      sysio::snapshot_attestation_table_id());
   abi.tables.emplace_back(
      snapshot_attest::table_snapconfig,
      snapshot_attest::index_type_i64,
      std::vector<sysio::chain::field_name>{snapshot_attest::field::name},
      std::vector<sysio::chain::type_name>{snapshot_attest::abi_type::name},
      snapshot_attest::type_snap_config,
      sysio::snapshot_attestation_config_table_id());
   abi.structs.emplace_back(
      snapshot_attest::type_snap_record,
      "",
      std::vector<sysio::chain::field_def>{
         {snapshot_attest::field::block_num, snapshot_attest::abi_type::uint32},
         {snapshot_attest::field::block_id, snapshot_attest::abi_type::checksum256},
         {snapshot_attest::field::snapshot_hash, snapshot_attest::abi_type::checksum256},
         {snapshot_attest::field::attested_at_block, snapshot_attest::abi_type::uint32},
      });
   abi.structs.emplace_back(
      snapshot_attest::type_snap_config,
      "",
      std::vector<sysio::chain::field_def>{
         {snapshot_attest::field::min_providers, snapshot_attest::abi_type::uint32},
      });
   return abi;
}

/** Initialize chain_plugin with one snapshot response-size limit override. */
sysio::chain::exit_code::exit_code initialize_with_snapshot_size_limit(std::string_view option_name,
                                                                       std::string_view option_value) {
   fc::temp_directory tmp;
   sysio::chain::application exe({.enable_resource_monitor = false});

   const auto tmp_path = tmp.path().string();
   std::vector<std::string> arguments{
      chain_plugin_test_program_name,
      "--snapshot-endpoint",
      "http://127.0.0.1:1",
      config_dir_option_name,
      tmp_path,
      data_dir_option_name,
      tmp_path,
      "--" + std::string(option_name),
      std::string(option_value),
   };
   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) {
      argv.push_back(argument.data());
   }

   return exe.init<sysio::chain_plugin>(static_cast<int>(argv.size()), argv.data());
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(chain_plugin_default_tests) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({
      .enable_resource_monitor = false
   });

   auto tmp_path = tmp.path().string();
   std::array          args = {
       chain_plugin_test_program_name, "--blocks-log-stride", "10", config_dir_option_name,
       tmp_path.c_str(), data_dir_option_name, tmp_path.c_str(),
   };

   BOOST_CHECK(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data())) == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();

   auto* config = std::get_if<sysio::chain::partitioned_blocklog_config>(&plugin.chain_config().blog);
   BOOST_REQUIRE(config);
   BOOST_CHECK_EQUAL(config->max_retained_files, UINT32_MAX);
   BOOST_CHECK_EQUAL(config->stride, 10);

}

/** Table-read failure retries through sync/grace before normal missing-record trust policy applies. */
BOOST_AUTO_TEST_CASE(snapshot_attestation_table_read_failure_policy) {
   using status = sysio::snapshot_attestation_table_read_status;
   using action = sysio::snapshot_attestation_table_read_action;

   BOOST_CHECK(sysio::classify_snapshot_attestation_table_read(status::success, true, false)
               == action::inspect_result);
   BOOST_CHECK(sysio::classify_snapshot_attestation_table_read(status::failure, false, false)
               == action::retry);
   BOOST_CHECK(sysio::classify_snapshot_attestation_table_read(status::failure, true, true)
               == action::retry);
   BOOST_CHECK(sysio::classify_snapshot_attestation_table_read(status::failure, true, false)
               == action::apply_missing_record_policy);
}

/** Internal attestation reads retain a decoder budget when the operator ABI timeout is too small. */
BOOST_AUTO_TEST_CASE(snapshot_attestation_table_read_timeout_policy) {
   constexpr fc::microseconds disabled_timeout{0};
   constexpr auto below_minimum =
      sysio::snapshot_attestation_minimum_table_read_timeout - fc::microseconds{1};
   constexpr auto above_minimum =
      sysio::snapshot_attestation_minimum_table_read_timeout
      + sysio::snapshot_attestation_minimum_table_read_timeout;

   BOOST_CHECK(sysio::snapshot_attestation_table_read_timeout(disabled_timeout)
               == sysio::snapshot_attestation_minimum_table_read_timeout);
   BOOST_CHECK(sysio::snapshot_attestation_table_read_timeout(below_minimum)
               == sysio::snapshot_attestation_minimum_table_read_timeout);
   BOOST_CHECK(sysio::snapshot_attestation_table_read_timeout(above_minimum)
               == above_minimum);
}

/** Require the table keys and ordered field prefix consumed by bootstrap verification. */
BOOST_AUTO_TEST_CASE(snapshot_attestation_required_schema_policy) {
   const auto valid_abi = make_snapshot_attestation_abi();
   BOOST_REQUIRE(sysio::has_required_snapshot_attestation_schema(valid_abi));

   auto extended_record = valid_abi;
   extended_record.structs.front().fields.push_back({"future_extension", snapshot_attest::abi_type::uint64});
   BOOST_CHECK(sysio::has_required_snapshot_attestation_schema(extended_record));

   auto wrong_index = valid_abi;
   wrong_index.tables.front().index_type = incompatible_index_type;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_index));

   auto wrong_table_id = valid_abi;
   wrong_table_id.tables.front().table_id = incompatible_table_id;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_table_id));

   auto wrong_key = valid_abi;
   wrong_key.tables.front().key_types.front() = snapshot_attest::abi_type::uint32;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_key));

   auto wrong_key_name = valid_abi;
   wrong_key_name.tables.front().key_names.front() = snapshot_attest::field::block_id;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_key_name));

   auto wrong_record_type = valid_abi;
   wrong_record_type.tables.front().type = incompatible_record_type;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_record_type));

   auto missing_field = valid_abi;
   missing_field.structs.front().fields.pop_back();
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(missing_field));

   auto wrong_field_type = valid_abi;
   wrong_field_type.structs.front().fields[1].type = snapshot_attest::abi_type::uint64;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_field_type));

   auto reordered_fields = valid_abi;
   std::swap(reordered_fields.structs.front().fields[1], reordered_fields.structs.front().fields[2]);
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(reordered_fields));

   auto missing_config_table = valid_abi;
   missing_config_table.tables.pop_back();
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(missing_config_table));

   auto wrong_config_table_id = valid_abi;
   wrong_config_table_id.tables.back().table_id = incompatible_table_id;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_config_table_id));

   auto wrong_config_field = valid_abi;
   wrong_config_field.structs.back().fields.front().type = snapshot_attest::abi_type::uint64;
   BOOST_CHECK(!sysio::has_required_snapshot_attestation_schema(wrong_config_field));
}

/** Recognize only one decoded snapconfig row with a positive fixed K. */
BOOST_AUTO_TEST_CASE(snapshot_attestation_enabled_config_policy) {
   BOOST_CHECK(sysio::snapshot_attestation_config_is_enabled(
      {fc::mutable_variant_object()(snapshot_attest::field::min_providers, 1)}));
   BOOST_CHECK(!sysio::snapshot_attestation_config_is_enabled({}));
   BOOST_CHECK(!sysio::snapshot_attestation_config_is_enabled(
      {fc::mutable_variant_object()(snapshot_attest::field::min_providers, 0)}));
   BOOST_CHECK(!sysio::snapshot_attestation_config_is_enabled({fc::variant{"malformed"}}));
}

/** Match both the loaded block id and root hash against the attested tuple. */
BOOST_AUTO_TEST_CASE(snapshot_attestation_record_tuple_policy) {
   const auto loaded_block_id = fc::sha256::hash(std::string{loaded_block_seed});
   const auto other_block_id = fc::sha256::hash(std::string{other_block_seed});
   const fc::crypto::blake3 loaded_hash(loaded_snapshot_hash);
   const fc::crypto::blake3 other_hash(other_snapshot_hash);

   BOOST_CHECK(sysio::snapshot_attestation_record_matches(
      loaded_block_id, loaded_hash, loaded_block_id, loaded_hash.str()));
   BOOST_CHECK(!sysio::snapshot_attestation_record_matches(
      loaded_block_id, loaded_hash, other_block_id, loaded_hash.str()));
   BOOST_CHECK(!sysio::snapshot_attestation_record_matches(
      loaded_block_id, loaded_hash, loaded_block_id, other_hash.str()));
}

/** Require endpoint metadata to identify the loaded scheduled snapshot head exactly. */
BOOST_AUTO_TEST_CASE(snapshot_endpoint_block_identity_policy) {
   constexpr uint32_t scheduled_block = snapshot_attest::block_spacing;
   constexpr uint32_t other_scheduled_block = snapshot_attest::block_spacing * 2;
   constexpr uint32_t unscheduled_block = scheduled_block + 1;
   constexpr uint64_t first_unsupported_block =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;

   BOOST_CHECK(sysio::snapshot_endpoint_block_matches(scheduled_block, scheduled_block));
   BOOST_CHECK(!sysio::snapshot_endpoint_block_matches(scheduled_block, other_scheduled_block));
   BOOST_CHECK(!sysio::snapshot_endpoint_block_matches(unscheduled_block, unscheduled_block));
   BOOST_CHECK(sysio::snapshot_endpoint_request_matches(std::nullopt, scheduled_block));
   BOOST_CHECK(sysio::snapshot_endpoint_request_matches(
      std::optional<uint32_t>{scheduled_block}, scheduled_block));
   BOOST_CHECK(!sysio::snapshot_endpoint_request_matches(
      std::optional<uint32_t>{scheduled_block}, other_scheduled_block));

   const auto parsed_scheduled_block =
      sysio::parse_snapshot_endpoint_block_num(std::to_string(scheduled_block));
   BOOST_REQUIRE(parsed_scheduled_block);
   BOOST_CHECK_EQUAL(*parsed_scheduled_block, scheduled_block);

   const auto parsed_maximum_block = sysio::parse_snapshot_endpoint_block_num(
      std::to_string(std::numeric_limits<uint32_t>::max()));
   BOOST_REQUIRE(parsed_maximum_block);
   BOOST_CHECK_EQUAL(*parsed_maximum_block, std::numeric_limits<uint32_t>::max());

   BOOST_CHECK(!sysio::parse_snapshot_endpoint_block_num(std::to_string(first_unsupported_block)));
   BOOST_CHECK(!sysio::parse_snapshot_endpoint_block_num(invalid_block_segment));
}

/** Auto-fetch a scheduled snapshot, replay its later attestation, and finish verification. */
BOOST_FIXTURE_TEST_CASE(
   chain_plugin_accepts_attested_auto_fetched_snapshot,
   snapshot_attest_test_support::snapshot_attest_fixture) {
   set_snap_config(snapshot_attest_test_support::single_provider_minimum);
   control->abort_block();

   const auto snapshot_block_num = control->head().block_num();
   const auto snapshot_block_id = control->head().id();
   BOOST_REQUIRE_EQUAL(snapshot_block_num, snapshot_attest::block_spacing);

   fc::temp_directory snapshot_dir;
   const auto snapshot_path = snapshot_dir.path() / valid_snapshot_filename;
   const auto snapshot_root_hash = write_chain_plugin_snapshot(*control, snapshot_path);
   const auto contract_hash =
      snapshot_attest_test_support::to_contract_snapshot_hash(snapshot_root_hash);

   produce_block();
   vote_snapshot(snapshot_attest_test_support::snapshot_provider_account,
                 snapshot_block_id, contract_hash);
   const auto attestation_block_num = produce_block()->block_num();
   produce_blocks(attestation_finality_block_count);
   BOOST_REQUIRE_GT(last_irreversible_block_num(), attestation_block_num);

   const auto source_blocks_path = get_config().blocks_dir.string();
   validate_and_close();

   snapshot_endpoint_server endpoint(snapshot_block_num, snapshot_root_hash, snapshot_path);
   fc::temp_directory node_dir;
   sysio::chain::application exe({.enable_resource_monitor = false});
   const auto node_path = node_dir.path().string();
   const auto endpoint_url = endpoint.url() + url_path_separator + std::to_string(snapshot_block_num);
   std::array args{
      chain_plugin_test_program_name,
      snapshot_endpoint_option_name,
      endpoint_url.c_str(),
      blocks_dir_option_name,
      source_blocks_path.c_str(),
      config_dir_option_name,
      node_path.c_str(),
      data_dir_option_name,
      node_path.c_str(),
   };

   BOOST_REQUIRE(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data()))
                 == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();
   BOOST_REQUIRE(!plugin.has_pending_snapshot_attestation());
   plugin.plugin_startup();

   BOOST_CHECK_GE(plugin.chain().head().block_num(), attestation_block_num);
   BOOST_CHECK(!plugin.has_pending_snapshot_attestation());
   BOOST_CHECK(!appbase::app().is_quiting());
   plugin.plugin_shutdown();
}

/** Reject an auto-fetched snapshot before replay when governance has not enabled attestation. */
BOOST_FIXTURE_TEST_CASE(
   chain_plugin_rejects_auto_fetched_snapshot_without_enabled_config,
   snapshot_attest_test_support::snapshot_attest_fixture) {
   produce_block();
   control->abort_block();

   const auto snapshot_block_num = control->head().block_num();
   BOOST_REQUIRE_EQUAL(snapshot_block_num, snapshot_attest::block_spacing);

   fc::temp_directory snapshot_dir;
   const auto snapshot_path = snapshot_dir.path() / unconfigured_snapshot_filename;
   const auto snapshot_root_hash = write_chain_plugin_snapshot(*control, snapshot_path);
   const auto source_blocks_path = get_config().blocks_dir.string();
   validate_and_close();

   snapshot_endpoint_server endpoint(snapshot_block_num, snapshot_root_hash, snapshot_path);
   fc::temp_directory node_dir;
   sysio::chain::application exe({.enable_resource_monitor = false});
   const auto node_path = node_dir.path().string();
   const auto endpoint_url = endpoint.url();
   std::array args{
      chain_plugin_test_program_name,
      snapshot_endpoint_option_name,
      endpoint_url.c_str(),
      blocks_dir_option_name,
      source_blocks_path.c_str(),
      config_dir_option_name,
      node_path.c_str(),
      data_dir_option_name,
      node_path.c_str(),
   };

   BOOST_REQUIRE(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data()))
                 == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();
   BOOST_CHECK_EXCEPTION(
      plugin.plugin_startup(), sysio::chain::plugin_config_exception,
      [](const sysio::chain::plugin_config_exception& error) {
         return error.to_detail_string().find(unconfigured_attestation_error_fragment)
                != std::string::npos;
      });
}

/** A manual --snapshot is an operator-trusted escape hatch without an attestation schema gate. */
BOOST_AUTO_TEST_CASE(chain_plugin_accepts_trusted_manual_snapshot_without_attestation_table) {
   sysio::testing::tester source(sysio::testing::setup_policy::full);

   sysio::chain::abi_def system_abi;
   const auto* system_account = source.control->find_account_metadata(
      sysio::chain::config::system_account_name);
   BOOST_REQUIRE(system_account != nullptr);
   BOOST_REQUIRE(sysio::chain::abi_serializer::to_abi(system_account->abi,
                                                       system_abi));
   BOOST_REQUIRE(std::none_of(
      system_abi.tables.begin(), system_abi.tables.end(),
      [](const sysio::chain::table_def& table) {
         return table.name == snapshot_attest::table_snaprecords;
      }));
   source.produce_blocks(snapshot_source_block_count);
   source.control->abort_block();
   const auto snapshot_block_num = source.head().block_num();

   fc::temp_directory snapshot_dir;
   const auto snapshot_path = snapshot_dir.path() / snapshot_without_attestation_table_filename;
   auto writer = std::make_shared<sysio::chain::threaded_snapshot_writer>(snapshot_path);
   source.control->write_snapshot(writer);
   // chain_plugin enables root-extension tracking while this system-contract fixture does not.
   // Supply the empty section that a snapshot from a production node always contains.
   writer->write_section<sysio::chain::contract_root_object>([](auto&) {});
   writer->finalize();

   const auto source_blocks_path = source.get_config().blocks_dir.string();
   source.close();

   fc::temp_directory node_dir;
   sysio::chain::application exe({.enable_resource_monitor = false});
   const auto node_path = node_dir.path().string();
   const auto snapshot_path_string = snapshot_path.string();
   std::array args{
      chain_plugin_test_program_name,
      snapshot_option_name,
      snapshot_path_string.c_str(),
      blocks_dir_option_name,
      source_blocks_path.c_str(),
      config_dir_option_name,
      node_path.c_str(),
      data_dir_option_name,
      node_path.c_str(),
   };

   BOOST_REQUIRE(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data()))
                 == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();
   plugin.plugin_startup();
   BOOST_CHECK_EQUAL(plugin.chain().head().block_num(), snapshot_block_num);
   BOOST_CHECK(!plugin.has_pending_snapshot_attestation());
   BOOST_CHECK(!appbase::app().is_quiting());
   plugin.plugin_shutdown();
}

/** Verify snapshot endpoint registration and removal of endpoint-specific resource knobs. */
BOOST_AUTO_TEST_CASE(chain_plugin_snapshot_endpoint_option_registration) {
   sysio::chain::application exe({.enable_resource_monitor = false});
   sysio::chain_plugin plugin;
   boost::program_options::options_description cli;
   boost::program_options::options_description cfg;
   boost::program_options::options_description options;
   plugin.set_program_options(cli, cfg);
   options.add(cli).add(cfg);

   std::array arguments{
      chain_plugin_test_program_name,
      "--snapshot-endpoint", "http://127.0.0.1:1",
   };
   boost::program_options::variables_map variables;
   boost::program_options::store(
      boost::program_options::parse_command_line(arguments.size(), const_cast<char**>(arguments.data()), options),
      variables);
   boost::program_options::notify(variables);

   BOOST_CHECK_EQUAL(variables.at("snapshot-endpoint").as<std::string>(), "http://127.0.0.1:1");
   BOOST_CHECK(options.find_nothrow("snapshot-endpoint-additional-ca-file", false) != nullptr);
   BOOST_CHECK(options.find_nothrow("snapshot-endpoint-additional-ca-path", false) != nullptr);
   BOOST_CHECK(options.find_nothrow("snapshot-endpoint-proxy", false) != nullptr);

   constexpr std::array removed_options{
      "snapshot-endpoint-connect-timeout-ms",
      "snapshot-endpoint-header-timeout-ms",
      "snapshot-endpoint-idle-timeout-ms",
      "snapshot-endpoint-total-timeout-ms",
      "snapshot-endpoint-max-download-size-mb",
      "snapshot-endpoint-min-disk-free-mb",
   };
   for (const auto* option_name : removed_options) {
      BOOST_CHECK(options.find_nothrow(option_name, false) == nullptr);
   }
}

/** Process-wide transport fallbacks remain unambiguous after caller option sets are aggregated. */
BOOST_AUTO_TEST_CASE(outbound_http_global_option_registration) {
   namespace bpo = boost::program_options;
   bpo::options_description global;
   bpo::options_description ethereum;
   bpo::options_description solana;
   bpo::options_description debugging;
   bpo::options_description signing;
   bpo::options_description snapshot;
   bpo::options_description options;

   sysio::outbound_http::add_global_transport_program_options(global);
   sysio::outbound_http::add_transport_program_options(
      ethereum,
      {"outpost-ethereum-additional-ca-file", "outpost-ethereum-additional-ca-path", "outpost-ethereum-proxy"},
      "Ethereum RPC");
   sysio::outbound_http::add_transport_program_options(
      solana,
      {"outpost-solana-additional-ca-file", "outpost-solana-additional-ca-path", "outpost-solana-proxy"},
      "Solana RPC");
   sysio::outbound_http::add_transport_program_options(
      debugging,
      {"ext-debugging-additional-ca-file", "ext-debugging-additional-ca-path", "ext-debugging-proxy"},
      "external-debugging");
   sysio::outbound_http::add_transport_program_options(
      signing,
      {"http-client-additional-ca-file", "http-client-additional-ca-path", "http-client-proxy"},
      "shared KIOD/signing");
   sysio::outbound_http::add_transport_program_options(
      snapshot,
      {"snapshot-endpoint-additional-ca-file", "snapshot-endpoint-additional-ca-path", "snapshot-endpoint-proxy"},
      "snapshot endpoint");
   options.add(global).add(ethereum).add(solana).add(debugging).add(signing).add(snapshot);

   std::array arguments{
      chain_plugin_test_program_name,
      "--outbound-http-additional-ca-file", "/tmp/wire-global-ca.pem",
      "--outbound-http-additional-ca-path", "/tmp/wire-global-ca",
      "--outbound-http-proxy", "http://127.0.0.1:3128",
      "--outpost-solana-additional-ca-file", "/tmp/wire-solana-ca.pem",
   };
   bpo::variables_map variables;
   bpo::store(
      bpo::parse_command_line(arguments.size(), const_cast<char**>(arguments.data()), options),
      variables);
   bpo::notify(variables);

   BOOST_CHECK_EQUAL(
      variables.at("outbound-http-additional-ca-file").as<std::filesystem::path>(),
      std::filesystem::path("/tmp/wire-global-ca.pem"));
   BOOST_CHECK_EQUAL(
      variables.at("outbound-http-additional-ca-path").as<std::filesystem::path>(),
      std::filesystem::path("/tmp/wire-global-ca"));
   BOOST_CHECK_EQUAL(
      variables.at("outbound-http-proxy").as<std::string>(),
      "http://127.0.0.1:3128");

   const auto solana_options =
      sysio::outbound_http::read_transport_options(
         variables,
         {"outpost-solana-additional-ca-file", "outpost-solana-additional-ca-path", "outpost-solana-proxy"});
   BOOST_REQUIRE(solana_options.additional_ca_file);
   BOOST_CHECK_EQUAL(*solana_options.additional_ca_file, std::filesystem::path("/tmp/wire-solana-ca.pem"));
   BOOST_REQUIRE(solana_options.additional_ca_path);
   BOOST_CHECK_EQUAL(*solana_options.additional_ca_path, std::filesystem::path("/tmp/wire-global-ca"));
   BOOST_REQUIRE(solana_options.proxy);
   BOOST_CHECK_EQUAL(*solana_options.proxy, "http://127.0.0.1:3128");
}

/** Verify that the snapshot response-size limit rejects zero and overflow before connecting. */
BOOST_AUTO_TEST_CASE(chain_plugin_snapshot_endpoint_option_validation) {
   constexpr std::array invalid_options{
      std::pair{"chain-state-db-size-mb", "0"},
      std::pair{"chain-state-db-size-mb", "17592186044416"},
   };

   for (const auto& [option_name, option_value] : invalid_options) {
      BOOST_TEST_CONTEXT(option_name << '=' << option_value) {
         BOOST_CHECK(initialize_with_snapshot_size_limit(option_name, option_value) != sysio::chain::exit_code::SUCCESS);
      }
   }
}

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
/** Verify the default SYS VM OC whitelist when the OC runtime is compiled into this build. */
BOOST_AUTO_TEST_CASE(chain_plugin_default_sys_vm_oc_whitelist) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({
      .enable_resource_monitor = false
   });

   auto tmp_path = tmp.path().string();
   std::array          args = {
       chain_plugin_test_program_name, config_dir_option_name, tmp_path.c_str(),
       data_dir_option_name, tmp_path.c_str(),
   };

   BOOST_CHECK(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data())) == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();

   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"wire"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"core.wire"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs.wire"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"vault"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{""}));
}

/** Verify command-line overrides for the SYS VM OC whitelist on builds that include the OC runtime. */
BOOST_AUTO_TEST_CASE(chain_plugin_sys_vm_oc_whitelist) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({
      .enable_resource_monitor = false
   });

   auto tmp_path = tmp.path().string();
   std::array          args = {
      chain_plugin_test_program_name, "--sys-vm-oc-whitelist", "hello", config_dir_option_name,
      tmp_path.c_str(), data_dir_option_name, tmp_path.c_str(),
  };

   BOOST_CHECK(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data())) == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"hello"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs.hello"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"wire"}));
}
#endif
