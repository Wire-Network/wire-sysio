#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/opp.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;

/// v6 data-model: reserves are keyed by the triple `(chain_code, token_code,
/// reserve_code)` (each a `sysio::slug_name` packed uint64). The legacy
/// `setreserve` action is gone; `regreserve` is the bootstrap-window
/// equivalent (it works only while `current_epoch_index == 0`, which is the
/// state immediately after deploying the contract in these tests).
class sysio_reserve_tester : public tester {
public:
   static constexpr auto RESERVE_ACCOUNT = "sysio.reserv"_n;
   static constexpr auto MSGCH_ACCOUNT   = "sysio.msgch"_n;

   sysio_reserve_tester() {
      produce_blocks(2);
      create_accounts({RESERVE_ACCOUNT, MSGCH_ACCOUNT});
      produce_blocks(2);

      set_code(RESERVE_ACCOUNT, contracts::reserve_wasm());
      set_abi(RESERVE_ACCOUNT, contracts::reserve_abi().data());
      set_privileged(RESERVE_ACCOUNT);
      produce_blocks();

      const auto* accnt = control->find_account_metadata(RESERVE_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);
      action act;
      act.account = RESERVE_ACCOUNT;
      act.name = action_name;
      act.data = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      act.authorization = vector<permission_level>{{signer, config::active_name}};

      signed_transaction trx;
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      trx.sign(get_private_key(signer, "active"), control->get_chain_id());
      try {
         push_transaction(trx);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   // ── SlugName helpers (v6) ──

   static fc::slug_name cn(std::string_view s) { return fc::slug_name{s}; }
   static fc::mutable_variant_object codename_mvo(std::string_view s) {
      return mvo()("value", fc::slug_name{s}.value);
   }

   /// `regreserve` is the v6 bootstrap-window action for inserting a reserve
   /// row with `status=ACTIVE`. Triple-slug_name PK is
   /// `(chain_code, token_code, reserve_code)`.
   action_result regreserve(std::string_view chain_code,
                            std::string_view token_code,
                            std::string_view reserve_code,
                            uint64_t initial_chain_amount,
                            uint64_t initial_wire_amount,
                            uint32_t weight = 5000,
                            const std::string& name_str = "test reserve",
                            const std::string& description = "") {
      return push_action(RESERVE_ACCOUNT, "regreserve"_n, mvo()
         ("chain_code",            codename_mvo(chain_code))
         ("token_code",            codename_mvo(token_code))
         ("reserve_code",          codename_mvo(reserve_code))
         ("name",                  name_str)
         ("description",           description)
         ("initial_chain_amount",  initial_chain_amount)
         ("initial_wire_amount",   initial_wire_amount)
         ("connector_weight_bps",  weight));
   }

   /// `onreward` v6 signature: `(chain_code, token_code, reserve_code,
   /// outpost_amount)`.
   action_result onreward(name signer,
                          std::string_view chain_code,
                          std::string_view token_code,
                          std::string_view reserve_code,
                          uint64_t outpost_amount) {
      return push_action(signer, "onreward"_n, mvo()
         ("chain_code",     codename_mvo(chain_code))
         ("token_code",     codename_mvo(token_code))
         ("reserve_code",   codename_mvo(reserve_code))
         ("outpost_amount", outpost_amount));
   }

   /// `onreject` v6 signature.
   action_result onreject(name signer,
                          std::string_view chain_code,
                          std::string_view token_code,
                          std::string_view reserve_code,
                          uint64_t unremitted_amount) {
      return push_action(signer, "onreject"_n, mvo()
         ("original_swap_remit_id", std::string(64, '0'))
         ("chain_code",             codename_mvo(chain_code))
         ("token_code",             codename_mvo(token_code))
         ("reserve_code",           codename_mvo(reserve_code))
         ("unremitted_amount",      unremitted_amount)
         ("recipient_address",      std::vector<char>{})
         ("reason",                 "test rejection"));
   }

   /// Walk every row in `sysio.reserv::reserves` (KV-keyed by checksum256)
   /// via the DB index and return the row whose slug_name triple matches.
   /// `get_row_by_id` only supports uint64 keys; this scan is the test-side
   /// workaround.
   fc::variant find_reserve(std::string_view chain_code,
                            std::string_view token_code,
                            std::string_view reserve_code) {
      const auto target_chain   = cn(chain_code).value;
      const auto target_token   = cn(token_code).value;
      const auto target_reserve = cn(reserve_code).value;

      const auto& db = control->db();
      const auto table_id = chain::compute_table_id("reserves"_n.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto itr = kv_idx.lower_bound(boost::make_tuple(RESERVE_ACCOUNT, table_id, std::string_view{}));
      for (; itr != kv_idx.end()
             && itr->code == RESERVE_ACCOUNT
             && itr->table_id == table_id; ++itr) {
         std::vector<char> raw(itr->value.size());
         if (!raw.empty())
            std::memcpy(raw.data(), itr->value.data(), raw.size());
         try {
            auto row = abi_ser.binary_to_variant(
               "reserve_row", raw,
               abi_serializer::create_yield_function(abi_serializer_max_time));
            if (row["chain_code"]["value"].as_uint64()   == target_chain &&
                row["token_code"]["value"].as_uint64()   == target_token &&
                row["reserve_code"]["value"].as_uint64() == target_reserve) {
               return row;
            }
         } catch (...) {
            // skip rows that don't decode
         }
      }
      return fc::variant();
   }

   abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_reserve_tests)

// ── regreserve (v6 bootstrap-window action) ──

BOOST_FIXTURE_TEST_CASE(regreserve_creates_reserve_row, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY",
                 /*chain_amount*/ 1'000'000, /*wire_amount*/ 2'000'000));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(1'000'000, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(2'000'000, r["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(5000,      r["connector_weight_bps"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regreserve_rejects_duplicate, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOL", "SOL", "PRIMARY", 100, 200, 5000));

   // Re-call with the same triple must reject (regreserve only inserts).
   BOOST_REQUIRE(
      regreserve("SOL", "SOL", "PRIMARY", 999, 1234, 6000).find("already") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regreserve_rejects_invalid_connector_weight, sysio_reserve_tester) { try {
   BOOST_REQUIRE(
      regreserve("ETH", "ETH", "PRIMARY", 100, 100, 0)
         .find("connector_weight_bps") != std::string::npos);

   BOOST_REQUIRE(
      regreserve("ETH", "ETH", "PRIMARY2", 100, 100, 10001)
         .find("connector_weight_bps") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── onreward ──

BOOST_FIXTURE_TEST_CASE(onreward_requires_msgch_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE(onreward(RESERVE_ACCOUNT, "ETH", "ETH", "PRIMARY", 100)
      .find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_grows_outpost_reserve_only, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(success(),
      onreward(MSGCH_ACCOUNT, "ETH", "ETH", "PRIMARY", 100));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(1100, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1000, r["reserve_wire_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_silently_skips_unknown_reserve, sysio_reserve_tester) { try {
   // v6: onreward is dispatched from msgch; per
   // feedback_opp_handlers_never_throw.md it MUST NOT throw. An unknown
   // reserve simply logs + skips and the action returns success.
   BOOST_REQUIRE_EQUAL(success(),
      onreward(MSGCH_ACCOUNT, "ETH", "ETH", "MISSING", 100));

   auto r = find_reserve("ETH", "ETH", "MISSING");
   BOOST_REQUIRE(r.is_null());
} FC_LOG_AND_RETHROW() }

// ── onreject ──

BOOST_FIXTURE_TEST_CASE(onreject_requires_msgch_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE(onreject(RESERVE_ACCOUNT, "ETH", "ETH", "PRIMARY", 50)
      .find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreject_re_adds_unremitted_amount, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(),
      onreject(MSGCH_ACCOUNT, "ETH", "ETH", "PRIMARY", 50));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(1050, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1000, r["reserve_wire_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

// ── swapquote ──

BOOST_FIXTURE_TEST_CASE(swapquote_returns_zero_when_reserve_missing, sysio_reserve_tester) { try {
   // No regreserve — the row simply doesn't exist.
   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(r.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
