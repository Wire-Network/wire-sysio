#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/variant_object.hpp>

#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

struct response_test_fixture {
   /**
    * MOCK implementation of the logfile input API
    */
   struct mock_logfile_provider {
      mock_logfile_provider(response_test_fixture& fixture)
      :fixture(fixture)
      {}

      /**
       * Read the trace for a given block
       * @param block_height : the height of the data being read
       * @return empty optional if the data cannot be read OTHERWISE
       *         optional containing a 2-tuple of the block_trace and a flag indicating irreversibility
       * @throws bad_data_exception : if the data is corrupt in some way
       */
      get_block_t get_block(uint32_t height) {
         return fixture.mock_get_block(height);
      }
      response_test_fixture& fixture;
   };

   constexpr static auto default_mock_data_handler = [](const action_trace_v0& a) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      std::optional<fc::variant> ret;
      if (!a.return_value.empty()) {
         ret = fc::mutable_variant_object()("hex", fc::to_hex(a.return_value.data(), a.return_value.size()));
      }
      return {fc::mutable_variant_object()("hex", fc::to_hex(a.data.data(), a.data.size())), ret};
   };

   struct mock_data_handler_provider {
      mock_data_handler_provider(response_test_fixture& fixture)
      :fixture(fixture)
      {}

      // Writes the decoded fields mock_data_handler returns.  Production decodes with
      // abi_serializer::binary_to_json_stream; these tests have no ABI.
      void serialize_to_json_stream(const action_trace_v0& action, fc::json_writer& w) {
         auto [params, return_data] = fixture.mock_data_handler(action);
         if (!params.is_null()) {
            w.key("params");
            fc::to_json_stream(params, w);
         }
         if (return_data.has_value()) {
            w.key("return_data");
            fc::to_json_stream(*return_data, w);
         }
      }

      response_test_fixture& fixture;
   };

   using response_impl_type = request_handler<mock_logfile_provider, mock_data_handler_provider>;

   response_test_fixture()
   : response_impl(mock_logfile_provider(*this), mock_data_handler_provider(*this),
                   [](const std::string& msg ){ fc_dlog( fc::logger::default_logger(), "{}", msg );})
   {

   }

   /// The block trace response parsed back from its JSON, or a null variant when the block is missing.
   fc::variant get_block_trace( uint32_t block_height ) {
      const std::string json = get_block_trace_json( block_height );
      return json.empty() ? fc::variant() : fc::json::from_string( json );
   }

   std::string get_block_trace_json( uint32_t block_height ) {
      return response_impl.get_block_trace_json( block_height );
   }

   /// The transaction trace response parsed back from its JSON, or a null variant on a miss.
   fc::variant get_transaction_trace( const chain::transaction_id_type& trxid, uint32_t block_height ) {
      const std::string json = get_transaction_trace_json( trxid, block_height );
      return json.empty() ? fc::variant() : fc::json::from_string( json );
   }

   std::string get_transaction_trace_json( const chain::transaction_id_type& trxid, uint32_t block_height ) {
      return response_impl.get_transaction_trace_json( trxid, block_height );
   }

   std::optional<std::function<void(fc::json_writer&)>> get_block_trace_emitter( uint32_t block_height ) {
      return response_impl.get_block_trace_emitter( block_height );
   }

   std::optional<std::function<void(fc::json_writer&)>> get_transaction_trace_emitter( const chain::transaction_id_type& trxid, uint32_t block_height ) {
      return response_impl.get_transaction_trace_emitter( trxid, block_height );
   }

   // fixture data and methods
   std::function<get_block_t(uint32_t)> mock_get_block;
   std::function<std::tuple<fc::variant, std::optional<fc::variant>>(const action_trace_v0&)> mock_data_handler = default_mock_data_handler;

   response_impl_type response_impl;

};

BOOST_AUTO_TEST_SUITE(trace_responses)
   BOOST_FIXTURE_TEST_CASE(basic_empty_block_response, response_test_fixture)
   {
      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         {}
      };

      fc::variant expected_response = fc::mutable_variant_object()
         ("id", "b000000000000000000000000000000000000000000000000000000000000001")
         ("number", 1)
         ("previous_id", "0000000000000000000000000000000000000000000000000000000000000000")
         ("status", "pending")
         ("timestamp", "2025-01-01T00:00:00.000Z")
         ("producer", "bp.one")
         ("transaction_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("finality_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("transactions", fc::variants() )
      ;

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         return std::make_tuple(data_log_entry{block_trace}, false);
      };

      fc::variant actual_response = get_block_trace( 1 );

      BOOST_TEST(to_kv(expected_response) == to_kv(actual_response), boost::test_tools::per_element());
   }

   BOOST_FIXTURE_TEST_CASE(basic_block_response, response_test_fixture)
   {
      action_trace_v0 action_trace{};
      action_trace.global_sequence = 0;
      action_trace.receiver        = "receiver"_n;
      action_trace.account         = "contract"_n;
      action_trace.action          = "action"_n;
      action_trace.authorization   = {{ "alice"_n, "active"_n }};
      action_trace.data            = { 0x00, 0x01, 0x02, 0x03 };
      action_trace.return_value    = { 0x04, 0x05, 0x06, 0x07 };

      auto transaction_trace = transaction_trace_v0 {
         "0000000000000000000000000000000000000000000000000000000000000001"_h,
         std::vector<action_trace_v0> {
            action_trace
         },
         10,
         5,
         std::vector<chain::signature_type>{ chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 },
         1,
         chain::block_timestamp_type(0),
         std::optional<chain::block_id_type>{}
      };

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            transaction_trace
         }
      };

      fc::variant expected_response = fc::mutable_variant_object()
         ("id", "b000000000000000000000000000000000000000000000000000000000000001")
         ("number", 1)
         ("previous_id", "0000000000000000000000000000000000000000000000000000000000000000")
         ("status", "pending")
         ("timestamp", "2025-01-01T00:00:00.000Z")
         ("producer", "bp.one")
         ("transaction_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("finality_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("transactions", fc::variants({
            fc::mutable_variant_object()
               ("id", "0000000000000000000000000000000000000000000000000000000000000001")
               ("block_num", 1)
               ("block_time", chain::block_timestamp_type(0))
               ("producer_block_id", fc::variant())
               ("actions", fc::variants({
                  fc::mutable_variant_object()
                     ("action_ordinal", 0)
                     ("creator_action_ordinal", 0)
                     ("closest_unnotified_ancestor_action_ordinal", 0)
                     ("global_sequence", 0)
                     ("recv_sequence", 0)
                     ("code_sequence", 0)
                     ("abi_sequence", 0)
                     ("receiver", "receiver")
                     ("account", "contract")
                     ("name", "action")
                     ("authorization", fc::variants({
                        fc::mutable_variant_object()
                           ("actor", "alice")
                           ("permission", "active")
                     }))
                     ("data", "00010203")
                     ("return_value", "04050607")
                     ("params", fc::mutable_variant_object()
                        ("hex", "00010203"))
                     ("return_data", fc::mutable_variant_object()
                        ("hex", "04050607"))
               }))
               ("cpu_usage_us", 10)
               ("net_usage_words", 5)
               ("signatures", fc::variants({"SIG_K1_111111111111111111111111111111111111111111111111111111111111111116uk5ne"}))
               ("transaction_header", fc::mutable_variant_object()
                  ("expiration", "1970-01-01T00:00:00")
                  ("ref_block_num", 1)
                  ("ref_block_prefix", 0)
                  ("max_net_usage_words", 100)
                  ("max_cpu_usage_ms", 50)
                  ("delay_sec", 0)
               )
         }))
      ;

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      fc::variant actual_response = get_block_trace( 1 );

      BOOST_TEST(to_kv(expected_response) == to_kv(actual_response), boost::test_tools::per_element());
   }

   BOOST_FIXTURE_TEST_CASE(basic_block_response_no_params, response_test_fixture)
   {
      action_trace_v0 inner_action{};
      inner_action.global_sequence = 0;
      inner_action.receiver        = "receiver"_n;
      inner_action.account         = "contract"_n;
      inner_action.action          = "action"_n;
      inner_action.authorization   = {{ "alice"_n, "active"_n }};
      inner_action.data            = { 0x00, 0x01, 0x02, 0x03 };
      inner_action.return_value    = { 0x04, 0x05, 0x06, 0x07 };

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            {
               "0000000000000000000000000000000000000000000000000000000000000001"_h,
               std::vector<action_trace_v0> {
                  inner_action
               },
               10,
               5,
               std::vector<chain::signature_type>{ chain::signature_type() },
               { chain::time_point_sec(), 1, 0, 100, 50, 0 },
               1,
               chain::block_timestamp_type(0),
               std::optional<chain::block_id_type>{}
            }
         }
      };

      fc::variant expected_response = fc::mutable_variant_object()
         ("id", "b000000000000000000000000000000000000000000000000000000000000001")
         ("number", 1)
         ("previous_id", "0000000000000000000000000000000000000000000000000000000000000000")
         ("status", "pending")
         ("timestamp", "2025-01-01T00:00:00.000Z")
         ("producer", "bp.one")
         ("transaction_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("finality_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("transactions", fc::variants({
            fc::mutable_variant_object()
               ("id", "0000000000000000000000000000000000000000000000000000000000000001")
               ("block_num", 1)
               ("block_time", chain::block_timestamp_type(0))
               ("producer_block_id", fc::variant())
               ("actions", fc::variants({
                  fc::mutable_variant_object()
                     ("action_ordinal", 0)
                     ("creator_action_ordinal", 0)
                     ("closest_unnotified_ancestor_action_ordinal", 0)
                     ("global_sequence", 0)
                     ("recv_sequence", 0)
                     ("code_sequence", 0)
                     ("abi_sequence", 0)
                     ("receiver", "receiver")
                     ("account", "contract")
                     ("name", "action")
                     ("authorization", fc::variants({
                        fc::mutable_variant_object()
                           ("actor", "alice")
                           ("permission", "active")
                     }))
                     ("data", "00010203")
                     ("return_value", "04050607")
               }))
               ("cpu_usage_us", 10)
               ("net_usage_words", 5)
               ("signatures", fc::variants({"SIG_K1_111111111111111111111111111111111111111111111111111111111111111116uk5ne"}))
               ("transaction_header", fc::mutable_variant_object()
                  ("expiration", "1970-01-01T00:00:00")
                  ("ref_block_num", 1)
                  ("ref_block_prefix", 0)
                  ("max_net_usage_words", 100)
                  ("max_cpu_usage_ms", 50)
                  ("delay_sec", 0)
               )
         }))
      ;

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      // simulate an inability to parse the parameters and return_data
      mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         return {};
      };

      fc::variant actual_response = get_block_trace( 1 );

      BOOST_TEST(to_kv(expected_response) == to_kv(actual_response), boost::test_tools::per_element());
   }

   BOOST_FIXTURE_TEST_CASE(basic_block_response_unsorted, response_test_fixture)
   {
      action_trace_v0 at1{};
      at1.global_sequence = 1;
      at1.receiver        = "receiver"_n;
      at1.account         = "contract"_n;
      at1.action          = "action"_n;
      at1.authorization   = {{ "alice"_n, "active"_n }};
      at1.data            = { 0x01, 0x01, 0x01, 0x01 };
      at1.return_value    = { 0x05, 0x05, 0x05, 0x05 };

      action_trace_v0 at0{};
      at0.global_sequence = 0;
      at0.receiver        = "receiver"_n;
      at0.account         = "contract"_n;
      at0.action          = "action"_n;
      at0.authorization   = {{ "alice"_n, "active"_n }};
      at0.data            = { 0x00, 0x00, 0x00, 0x00 };
      at0.return_value    = { 0x04, 0x04, 0x04, 0x04 };

      action_trace_v0 at2{};
      at2.global_sequence = 2;
      at2.receiver        = "receiver"_n;
      at2.account         = "contract"_n;
      at2.action          = "action"_n;
      at2.authorization   = {{ "alice"_n, "active"_n }};
      at2.data            = { 0x02, 0x02, 0x02, 0x02 };
      at2.return_value    = { 0x06, 0x06, 0x06, 0x06 };

      std::vector<action_trace_v0> actions = { at1, at0, at2 };

      auto transaction_trace = transaction_trace_v0 {
         "0000000000000000000000000000000000000000000000000000000000000001"_h,
         actions,
         10,
         5,
         { chain::signature_type() },
         { chain::time_point_sec(), 1, 0, 100, 50, 0 },
         1,
         chain::block_timestamp_type(0),
         std::optional<chain::block_id_type>{}
      };

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            transaction_trace
         }
      };

      fc::variant expected_response = fc::mutable_variant_object()
         ("id", "b000000000000000000000000000000000000000000000000000000000000001")
         ("number", 1)
         ("previous_id", "0000000000000000000000000000000000000000000000000000000000000000")
         ("status", "pending")
         ("timestamp", "2025-01-01T00:00:00.000Z")
         ("producer", "bp.one")
         ("transaction_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("finality_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("transactions", fc::variants({
            fc::mutable_variant_object()
            ("id", "0000000000000000000000000000000000000000000000000000000000000001")
            ("block_num", 1)
            ("block_time", chain::block_timestamp_type(0))
            ("producer_block_id", fc::variant())
            ("actions", fc::variants({
               fc::mutable_variant_object()
                  ("action_ordinal", 0)
                  ("creator_action_ordinal", 0)
                  ("closest_unnotified_ancestor_action_ordinal", 0)
                  ("global_sequence", 0)
                  ("recv_sequence", 0)
                  ("code_sequence", 0)
                  ("abi_sequence", 0)
                  ("receiver", "receiver")
                  ("account", "contract")
                  ("name", "action")
                  ("authorization", fc::variants({
                     fc::mutable_variant_object()
                        ("actor", "alice")
                        ("permission", "active")
                     }))
                  ("data", "00000000")
                  ("return_value", "04040404")
               ,
               fc::mutable_variant_object()
                  ("action_ordinal", 0)
                  ("creator_action_ordinal", 0)
                  ("closest_unnotified_ancestor_action_ordinal", 0)
                  ("global_sequence", 1)
                  ("recv_sequence", 0)
                  ("code_sequence", 0)
                  ("abi_sequence", 0)
                  ("receiver", "receiver")
                  ("account", "contract")
                  ("name", "action")
                  ("authorization", fc::variants({
                     fc::mutable_variant_object()
                        ("actor", "alice")
                        ("permission", "active")
                     }))
                  ("data", "01010101")
                  ("return_value", "05050505")
               ,
               fc::mutable_variant_object()
                  ("action_ordinal", 0)
                  ("creator_action_ordinal", 0)
                  ("closest_unnotified_ancestor_action_ordinal", 0)
                  ("global_sequence", 2)
                  ("recv_sequence", 0)
                  ("code_sequence", 0)
                  ("abi_sequence", 0)
                  ("receiver", "receiver")
                  ("account", "contract")
                  ("name", "action")
                  ("authorization", fc::variants({
                     fc::mutable_variant_object()
                        ("actor", "alice")
                        ("permission", "active")
                        }))
                  ("data", "02020202")
                  ("return_value", "06060606")
                  }))
               ("cpu_usage_us", 10)
               ("net_usage_words", 5)
               ("signatures", fc::variants({"SIG_K1_111111111111111111111111111111111111111111111111111111111111111116uk5ne"}))
               ("transaction_header", fc::mutable_variant_object()
                  ("expiration", "1970-01-01T00:00:00")
                  ("ref_block_num", 1)
                  ("ref_block_prefix", 0)
                  ("max_net_usage_words", 100)
                  ("max_cpu_usage_ms", 50)
                  ("delay_sec", 0)
               )
      }))
      ;

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      // simulate an inability to parse the parameters and return_data
      mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
         return {};
      };

      fc::variant actual_response = get_block_trace( 1 );

      BOOST_TEST(to_kv(expected_response) == to_kv(actual_response), boost::test_tools::per_element());
   }

   BOOST_FIXTURE_TEST_CASE(lib_response, response_test_fixture)
   {
      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         {}
      };

      fc::variant expected_response = fc::mutable_variant_object()
         ("id", "b000000000000000000000000000000000000000000000000000000000000001")
         ("number", 1)
         ("previous_id", "0000000000000000000000000000000000000000000000000000000000000000")
         ("status", "irreversible")
         ("timestamp", "2025-01-01T00:00:00.000Z")
         ("producer", "bp.one")
         ("transaction_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("finality_mroot", "0000000000000000000000000000000000000000000000000000000000000000")
         ("transactions", fc::variants() )
      ;

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         return std::make_tuple(data_log_entry(block_trace), true);
      };

      fc::variant response = get_block_trace( 1 );
      BOOST_TEST(to_kv(expected_response) == to_kv(response), boost::test_tools::per_element());

   }

   BOOST_FIXTURE_TEST_CASE(corrupt_block_data, response_test_fixture)
   {
      mock_get_block = []( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         throw bad_data_exception("mock exception");
      };

      BOOST_REQUIRE_THROW(get_block_trace( 1 ), bad_data_exception);
   }

   BOOST_FIXTURE_TEST_CASE(missing_block_data, response_test_fixture)
   {
      mock_get_block = []( uint32_t height ) -> get_block_t {
         BOOST_TEST(height == 1u);
         return {};
      };

      fc::variant null_response = get_block_trace( 1 );

      BOOST_TEST(null_response.is_null());
   }

   // Exact block response bytes: field order, number formats and string encodings.  Catches silent shape
   // regressions like status: "executed" -> 0 or block_time: {"slot":N} -> N.
   BOOST_FIXTURE_TEST_CASE(block_response_exact_json, response_test_fixture)
   {
      action_trace_v0 action_trace{};
      action_trace.global_sequence = 0;
      action_trace.receiver        = "receiver"_n;
      action_trace.account         = "contract"_n;
      action_trace.action          = "action"_n;
      action_trace.authorization   = {{ "alice"_n, "active"_n }};
      action_trace.data            = { 0x00, 0x01, 0x02, 0x03 };
      action_trace.return_value    = { 0x04, 0x05, 0x06, 0x07 };

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            {
               "0000000000000000000000000000000000000000000000000000000000000001"_h,
               std::vector<action_trace_v0> { action_trace },
               10,
               5,
               std::vector<chain::signature_type>{ chain::signature_type() },
               { chain::time_point_sec(), 1, 0, 100, 50, 0 },
               1,
               chain::block_timestamp_type(0),
               std::optional<chain::block_id_type>{}
            }
         }
      };

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      BOOST_TEST(get_block_trace_json( 1 ) ==
                 R"json({"id":"b000000000000000000000000000000000000000000000000000000000000001","number":1,)json"
                 R"json("previous_id":"0000000000000000000000000000000000000000000000000000000000000000",)json"
                 R"json("status":"pending","timestamp":"2025-01-01T00:00:00.000Z","producer":"bp.one",)json"
                 R"json("transaction_mroot":"0000000000000000000000000000000000000000000000000000000000000000",)json"
                 R"json("finality_mroot":"0000000000000000000000000000000000000000000000000000000000000000",)json"
                 R"json("transactions":[{"id":"0000000000000000000000000000000000000000000000000000000000000001")json"
                 R"json(,"block_num":1,"block_time":"2025-01-01T00:00:00.000","producer_block_id":null,)json"
                 R"json("actions":[{"global_sequence":0,"receiver":"receiver","account":"contract",)json"
                 R"json("name":"action","authorization":[{"actor":"alice","permission":"active"}],)json"
                 R"json("data":"00010203","return_value":"04050607","action_ordinal":0,)json"
                 R"json("creator_action_ordinal":0,"closest_unnotified_ancestor_action_ordinal":0,)json"
                 R"json("recv_sequence":0,"auth_sequence":[],"code_sequence":0,"abi_sequence":0,)json"
                 R"json("account_ram_deltas":[],"params":{"hex":"00010203"},"return_data":{"hex":"04050607"}}],)json"
                 R"json("cpu_usage_us":10,"net_usage_words":5,"signatures":["SIG_K1_1111111111111111111111111111)json"
                 R"json(11111111111111111111111111111111111116uk5ne"],)json"
                 R"json("transaction_header":{"expiration":"1970-01-01T00:00:00","ref_block_num":1,)json"
                 R"json("ref_block_prefix":0,"max_net_usage_words":100,"max_cpu_usage_ms":50,"delay_sec":0}}]})json");
   }

   // Exact transaction response bytes for a matching trxid, and an empty response for a missing one.  Catches
   // shape regressions in the per-transaction emitter that the block-level test would not, because they are
   // masked by the surrounding block wrapper.
   BOOST_FIXTURE_TEST_CASE(transaction_response_exact_json, response_test_fixture)
   {
      auto trx_id_a = "0000000000000000000000000000000000000000000000000000000000000001"_h;
      auto trx_id_b = "0000000000000000000000000000000000000000000000000000000000000002"_h;

      action_trace_v0 action_a{};
      action_a.global_sequence = 0;
      action_a.receiver        = "receiver"_n;
      action_a.account         = "contract"_n;
      action_a.action          = "action"_n;
      action_a.authorization   = {{ "alice"_n, "active"_n }};
      action_a.data            = { 0x00, 0x01, 0x02, 0x03 };
      action_a.return_value    = { 0x04, 0x05, 0x06, 0x07 };

      // Populate every optional / container field the full shape emits, so the
      // assertion also covers the ordinal, auth_sequence, ram-delta and usage paths.
      action_trace_v0 action_b{};
      action_b.action_ordinal                             = 2;
      action_b.creator_action_ordinal                     = 1;
      action_b.closest_unnotified_ancestor_action_ordinal = 1;
      action_b.global_sequence                            = 1;
      action_b.recv_sequence                              = 7;
      action_b.auth_sequence                              = {{ "bob"_n, 3 }};
      action_b.code_sequence                              = 2;
      action_b.abi_sequence                               = 4;
      action_b.receiver                                   = "receiver"_n;
      action_b.account                                    = "contract"_n;
      action_b.action                                     = "action"_n;
      action_b.authorization                              = {{ "bob"_n, "active"_n }};
      action_b.data                                       = { 0x10, 0x11 };
      action_b.account_ram_deltas                         = {{ "bob"_n, 240 }};
      action_b.cpu_usage_us                               = 11;
      action_b.net_usage                                  = 8;

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            {
               trx_id_a,
               std::vector<action_trace_v0> { action_a },
               10,
               5,
               std::vector<chain::signature_type>{ chain::signature_type() },
               { chain::time_point_sec(), 1, 0, 100, 50, 0 },
               1,
               chain::block_timestamp_type(0),
               std::optional<chain::block_id_type>{}
            },
            {
               trx_id_b,
               std::vector<action_trace_v0> { action_b },
               20,
               6,
               std::vector<chain::signature_type>{ chain::signature_type() },
               { chain::time_point_sec(), 2, 0, 200, 60, 0 },
               1,
               chain::block_timestamp_type(0),
               std::optional<chain::block_id_type>{}
            }
         }
      };

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      // Hit case: only the matching transaction is emitted.
      BOOST_TEST(get_transaction_trace_json( trx_id_b, 1 ) ==
                 R"json({"id":"0000000000000000000000000000000000000000000000000000000000000002","block_num":1,)json"
                 R"json("block_time":"2025-01-01T00:00:00.000","producer_block_id":null,)json"
                 R"json("actions":[{"global_sequence":1,"receiver":"receiver","account":"contract",)json"
                 R"json("name":"action","authorization":[{"actor":"bob","permission":"active"}],"data":"1011",)json"
                 R"json("return_value":"","action_ordinal":2,"creator_action_ordinal":1,)json"
                 R"json("closest_unnotified_ancestor_action_ordinal":1,"recv_sequence":7,)json"
                 R"json("auth_sequence":[["bob",3]],"code_sequence":2,"abi_sequence":4,)json"
                 R"json("account_ram_deltas":[{"account":"bob","delta":240}],"cpu_usage_us":11,"net_usage":8,)json"
                 R"json("params":{"hex":"1011"}}],"cpu_usage_us":20,"net_usage_words":6,)json"
                 R"json("signatures":["SIG_K1_111111111111111111111111111111111111111111111111111111111111111116)json"
                 R"json(uk5ne"],"transaction_header":{"expiration":"1970-01-01T00:00:00","ref_block_num":2,)json"
                 R"json("ref_block_prefix":0,"max_net_usage_words":200,"max_cpu_usage_ms":60,"delay_sec":0}})json");

      // Miss case: trxid not in the block.
      auto missing_id = "00000000000000000000000000000000000000000000000000000000000000ff"_h;
      BOOST_TEST(get_transaction_trace_json( missing_id, 1 ).empty());
   }

   // The HTTP layer commits to a 200 before the emitter runs, so both emitter factories must
   // resolve every miss (absent block, absent trxid) to nullopt up front -- and an engaged
   // emitter must produce exactly the bytes the buffer-building wrappers produce.
   BOOST_FIXTURE_TEST_CASE(streaming_emitter_miss_resolution_and_parity, response_test_fixture)
   {
      auto trx_id = "0000000000000000000000000000000000000000000000000000000000000001"_h;

      action_trace_v0 action_trace{};
      action_trace.global_sequence = 0;
      action_trace.receiver        = "receiver"_n;
      action_trace.account         = "contract"_n;
      action_trace.action          = "action"_n;
      action_trace.authorization   = {{ "alice"_n, "active"_n }};
      action_trace.data            = { 0x00, 0x01, 0x02, 0x03 };

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::vector<transaction_trace_v0> {
            {
               trx_id,
               std::vector<action_trace_v0> { action_trace },
               10,
               5,
               std::vector<chain::signature_type>{ chain::signature_type() },
               { chain::time_point_sec(), 1, 0, 100, 50, 0 },
               1,
               chain::block_timestamp_type(0),
               std::optional<chain::block_id_type>{}
            }
         }
      };

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         if (height != 1) {
            return {};
         }
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      auto drive = [](const std::function<void(fc::json_writer&)>& emit) {
         std::string out;
         {
            fc::json_writer w(out);
            emit(w);
         }
         return out;
      };

      // Engaged emitters emit byte-identically to the wrappers.
      auto block_emitter = get_block_trace_emitter( 1 );
      BOOST_REQUIRE(block_emitter.has_value());
      BOOST_CHECK_EQUAL(drive(*block_emitter), get_block_trace_json( 1 ));

      auto trx_emitter = get_transaction_trace_emitter( trx_id, 1 );
      BOOST_REQUIRE(trx_emitter.has_value());
      BOOST_CHECK_EQUAL(drive(*trx_emitter), get_transaction_trace_json( trx_id, 1 ));

      // Misses resolve before any emitter exists: absent block, and absent trxid in a
      // present block (contains_transaction gating).
      BOOST_CHECK(!get_block_trace_emitter( 2 ).has_value());
      auto missing_id = "00000000000000000000000000000000000000000000000000000000000000ff"_h;
      BOOST_CHECK(!get_transaction_trace_emitter( missing_id, 1 ).has_value());
      BOOST_CHECK(!get_transaction_trace_emitter( trx_id, 2 ).has_value());
   }

   // Trace responses must be abortable by the http layer's memory budget WHILE they
   // serialize: driving the block emitter into a small-threshold guarded writer (the shape
   // make_http_stream_response_handler provides) must throw the guard's abort partway
   // through emission, with the buffer far below the full body size -- the old json_raw
   // path materialized the entire body before the budget ever saw a byte.
   BOOST_FIXTURE_TEST_CASE(streaming_block_response_budget_abort, response_test_fixture)
   {
      // 32 transactions x 4 KiB action payload: full body far exceeds the 8 KiB threshold
      // below (each payload alone hex-expands past it).
      constexpr size_t payload_bytes    = 4 * 1024;
      constexpr size_t transaction_count = 32;
      std::vector<transaction_trace_v0> transactions;
      transactions.reserve(transaction_count);
      for (size_t i = 0; i < transaction_count; ++i) {
         action_trace_v0 action_trace{};
         action_trace.global_sequence = i;
         action_trace.receiver        = "receiver"_n;
         action_trace.account         = "contract"_n;
         action_trace.action          = "action"_n;
         action_trace.authorization   = {{ "alice"_n, "active"_n }};
         action_trace.data.assign(payload_bytes, static_cast<char>(i));
         transactions.push_back(transaction_trace_v0{
            fc::sha256::hash(static_cast<uint64_t>(i)),
            std::vector<action_trace_v0> { std::move(action_trace) },
            10,
            5,
            std::vector<chain::signature_type>{ chain::signature_type() },
            { chain::time_point_sec(), 1, 0, 100, 50, 0 },
            1,
            chain::block_timestamp_type(0),
            std::optional<chain::block_id_type>{}
         });
      }

      auto block_trace = block_trace_v0 {
         "b000000000000000000000000000000000000000000000000000000000000001"_h,
         1,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         chain::block_timestamp_type(0),
         "bp.one"_n,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         "0000000000000000000000000000000000000000000000000000000000000000"_h,
         std::move(transactions)
      };

      mock_get_block = [&block_trace]( uint32_t height ) -> get_block_t {
         return std::make_tuple(data_log_entry(block_trace), false);
      };

      const std::string full_body = get_block_trace_json( 1 );

      /// Foreign (non-fc, non-std) exception type modeling the http layer's budget abort
      /// (sysio::detail::stream_response_budget_exceeded).
      struct budget_abort {};
      constexpr size_t threshold = 8 * 1024;

      auto emitter = get_block_trace_emitter( 1 );
      BOOST_REQUIRE(emitter.has_value());
      std::string out;
      fc::json_writer w(out, [](size_t sz) { if (sz > threshold) throw budget_abort{}; }, 256);
      BOOST_CHECK_THROW((*emitter)(w), budget_abort);
      // Aborted within a token or two of the threshold; nowhere near the full body.
      BOOST_CHECK_LT(out.size(), 2 * threshold);
      BOOST_CHECK_GT(full_body.size(), 8 * threshold);
   }

BOOST_AUTO_TEST_SUITE_END()
