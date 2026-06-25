// contracts/tests/sysio.tokens_tests.cpp
//
// Focus: sysio.tokens registry — the chain-token binding action `regctok`
// (contract address + is_native). Chain-native precision is not stored on the
// binding; the depot's per-token precision lives on Token.precision and the
// reserve row, and the outpost owns the chain-native ↔ depot-frame conversion.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include <cstring>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;

using mvo = fc::mutable_variant_object;

/// Minimal fixture: deploy the privileged sysio.tokens registry on a plain
/// (non-ROA) tester. With no sysio.epoch deployed, `current_epoch_index`
/// reads as 0 — the bootstrap window — so registrations come up `active`.
class sysio_tokens_tester : public tester {
public:
   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;

   sysio_tokens_tester() {
      produce_blocks(2);
      create_accounts({ TOKENS_ACCOUNT });
      produce_blocks(2);

      set_code(TOKENS_ACCOUNT, contracts::tokens_wasm());
      set_abi(TOKENS_ACCOUNT, contracts::tokens_abi().data());
      set_privileged(TOKENS_ACCOUNT);
      produce_blocks();

      const auto* accnt = control->find_account_metadata(TOKENS_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action(name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(TOKENS_ACCOUNT, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   static fc::mutable_variant_object codename(std::string_view s) {
      return mvo()("value", fc::slug_name{s}.value);
   }

   /// `chaintokens` is a uint128-keyed kv::table; `get_row_by_id` only supports
   /// uint64 keys, so walk the table by its DB index and match the slug pair
   /// (the same workaround sysio.reserv_tests uses for its checksum-keyed table).
   fc::variant find_chaintoken(std::string_view chain_code, std::string_view token_code) {
      const auto target_chain = fc::slug_name{chain_code}.value;
      const auto target_token = fc::slug_name{token_code}.value;

      const auto& db = control->db();
      const auto table_id = chain::compute_table_id("chaintokens"_n.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto itr = kv_idx.lower_bound(boost::make_tuple(TOKENS_ACCOUNT, table_id, std::string_view{}));
      for (; itr != kv_idx.end()
             && itr->code == TOKENS_ACCOUNT
             && itr->table_id == table_id; ++itr) {
         std::vector<char> raw(itr->value.size());
         if (!raw.empty())
            std::memcpy(raw.data(), itr->value.data(), raw.size());
         try {
            auto row = abi_ser.binary_to_variant(
               "chain_token_row", raw,
               abi_serializer::create_yield_function(abi_serializer_max_time));
            if (row["chain_code"]["value"].as_uint64() == target_chain &&
                row["token_code"]["value"].as_uint64() == target_token) {
               return row;
            }
         } catch (...) {
            // skip rows that don't decode
         }
      }
      return fc::variant();
   }

private:
   abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_tokens_tests)

// regctok records the (chain, token) binding (contract address + is_native),
// active inline during the bootstrap window. Chain-native precision is NOT
// stored on the binding — the depot's per-token precision lives on
// Token.precision and the reserve row; the outpost owns the boundary conversion.
BOOST_FIXTURE_TEST_CASE(regctok_records_binding, sysio_tokens_tester) { try {
   // Native binding.
   BOOST_REQUIRE_EQUAL(success(), push_action(TOKENS_ACCOUNT, "regctok"_n, mvo()
      ("chain_code",    codename("ETH"))
      ("token_code",    codename("WIRE"))
      ("contract_addr", "")
      ("is_native",     true)));

   auto native = find_chaintoken("ETH", "WIRE");
   BOOST_REQUIRE(!native.is_null());
   BOOST_REQUIRE_EQUAL(true, native["is_native"].as<bool>());
   BOOST_REQUIRE_EQUAL(true, native["active"].as<bool>()); // bootstrap window -> active

   // Non-native ERC-20 binding with a contract address.
   BOOST_REQUIRE_EQUAL(success(), push_action(TOKENS_ACCOUNT, "regctok"_n, mvo()
      ("chain_code",    codename("ETH"))
      ("token_code",    codename("USDC"))
      ("contract_addr", "01")
      ("is_native",     false)));

   auto erc20 = find_chaintoken("ETH", "USDC");
   BOOST_REQUIRE(!erc20.is_null());
   BOOST_REQUIRE_EQUAL(false, erc20["is_native"].as<bool>());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
