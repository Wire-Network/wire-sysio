#include <boost/test/unit_test.hpp>

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/crypto/sha256.hpp>

#include <optional>

#include "contracts.hpp"
#include "sysio.system_tester.hpp"

using namespace sysio::chain;
using namespace sysio::testing;
using namespace sysio_system;
using mvo = fc::mutable_variant_object;

namespace {

constexpr auto abihash_table  = "abihash"_n;
constexpr auto codehash_table = "codehash"_n;

/// A system account the fixture creates under sysio.bios, so it has unlimited resources.
constexpr auto tracked_account = "sysio.bpay"_n;
/// The memo of a `setcode` sysio.system must record without parsing.
constexpr auto deploy_memo     = "deploy sysio.msig";

/// Every row of one of sysio's ABI and code hash tables, decoded as `row_type`.
std::vector<fc::variant> table_rows(const base_tester& t, const abi_serializer& serializer, name table,
                                    const std::string& row_type) {
   const auto& index    = t.control->db().get_index<kv_index, by_code_key>();
   const auto  table_id = compute_table_id(table.to_uint64_t());
   std::vector<fc::variant> rows;
   for (auto it = index.lower_bound(boost::make_tuple(config::system_account_name, table_id));
        it != index.end() && it->code == config::system_account_name && it->table_id == table_id; ++it) {
      const std::vector<char> bytes(it->value.begin(), it->value.end());
      rows.push_back(serializer.binary_to_variant(row_type, bytes, base_tester::fixture_abi_yield_function()));
   }
   return rows;
}

/// The first of `rows` whose `owner` is `owner`.
std::optional<fc::variant> row_for(const std::vector<fc::variant>& rows, name owner) {
   for (const auto& row : rows)
      if (row["owner"].as<name>() == owner)
         return row;
   return std::nullopt;
}

/// The block time and number an action applied in the pending block sees.
struct block_stamp {
   fc::time_point time;
   uint32_t       num = 0;
};

block_stamp pending_stamp(const base_tester& t) {
   return {t.control->pending_block_time(), t.control->pending_block_num()};
}

void require_stamp(const fc::variant& row, const block_stamp& stamp) {
   BOOST_REQUIRE(row["updated_at"].as<fc::time_point>() == stamp.time);
   BOOST_REQUIRE_EQUAL(row["block_num"].as<uint32_t>(), stamp.num);
}

fc::sha256 wasm_hash(const std::vector<uint8_t>& wasm) {
   return fc::sha256::hash(reinterpret_cast<const char*>(wasm.data()), wasm.size());
}

fc::sha256 packed_abi_hash(const char* abi_json) {
   const auto packed = fc::raw::pack(fc::json::from_string(abi_json).as<abi_def>());
   return fc::sha256::hash(packed.data(), packed.size());
}

/// A `codehash` row holds `wasm`'s hash, which is also the code hash the chain holds for `owner`.
void require_code_hash(const base_tester& t, const fc::variant& row, name owner, const std::vector<uint8_t>& wasm) {
   BOOST_REQUIRE_EQUAL(row["hash"].as<fc::sha256>(), wasm_hash(wasm));
   BOOST_REQUIRE(row["hash"].as<fc::sha256>() == t.control->find_account_metadata(owner)->code_hash);
}

} // namespace

BOOST_AUTO_TEST_SUITE(sysio_account_changes_tests)

BOOST_FIXTURE_TEST_CASE(system_records_abi_hash_and_when, sysio_system_tester) try {
   set_abi(tracked_account, contracts::token_abi().data());
   const auto first = pending_stamp(*this);
   auto row = row_for(table_rows(*this, abi_ser, abihash_table, "abi_hash"), tracked_account);
   BOOST_REQUIRE(row);
   BOOST_REQUIRE_EQUAL((*row)["hash"].as<fc::sha256>(), packed_abi_hash(contracts::token_abi().data()));
   require_stamp(*row, first);

   // Setting the same ABI again keeps the hash and moves the time, so a reset between two reads still shows.
   produce_block();
   set_abi(tracked_account, contracts::token_abi().data());
   const auto second = pending_stamp(*this);
   row = row_for(table_rows(*this, abi_ser, abihash_table, "abi_hash"), tracked_account);
   BOOST_REQUIRE(row);
   BOOST_REQUIRE_EQUAL((*row)["hash"].as<fc::sha256>(), packed_abi_hash(contracts::token_abi().data()));
   BOOST_REQUIRE_GT(second.num, first.num);
   require_stamp(*row, second);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(system_records_code_hash_set_replaced_and_cleared, sysio_system_tester) try {
   const auto code_row = [&] {
      return row_for(table_rows(*this, abi_ser, codehash_table, "code_hash"), tracked_account);
   };

   set_code(tracked_account, contracts::token_wasm());
   auto stamp = pending_stamp(*this);
   auto row   = code_row();
   BOOST_REQUIRE(row);
   require_code_hash(*this, *row, tracked_account, contracts::token_wasm());
   require_stamp(*row, stamp);

   // A memo follows the code; sysio.system parses neither and still records the new hash.
   produce_block();
   const auto& msig = contracts::msig_wasm();
   base_tester::push_action(config::system_account_name, "setcode"_n, tracked_account,
                            mvo()("account", tracked_account)("vmtype", 0)("vmversion", 0)
                                 ("code", std::vector<char>(msig.begin(), msig.end()))("memo", deploy_memo));
   stamp = pending_stamp(*this);
   row   = code_row();
   BOOST_REQUIRE(row);
   require_code_hash(*this, *row, tracked_account, msig);
   require_stamp(*row, stamp);

   // Clearing the code keeps the row, with the chain's all-zero code hash.
   produce_block();
   set_code(tracked_account, std::vector<uint8_t>{});
   stamp = pending_stamp(*this);
   row   = code_row();
   BOOST_REQUIRE(row);
   BOOST_REQUIRE_EQUAL((*row)["hash"].as<fc::sha256>(), fc::sha256());
   BOOST_REQUIRE(control->find_account_metadata(tracked_account)->code_hash == digest_type());
   require_stamp(*row, stamp);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(system_contract_records_its_own_code, sysio_system_tester) try {
   const auto rows = table_rows(*this, abi_ser, codehash_table, "code_hash");

   // The system contract replaced sysio.bios on sysio; its own handler ran on the new code and recorded it.
   const auto system_row = row_for(rows, config::system_account_name);
   BOOST_REQUIRE(system_row);
   require_code_hash(*this, *system_row, config::system_account_name, contracts::system_wasm());

   // sysio.token was deployed while sysio.bios was on sysio, which recorded it.
   const auto token_row = row_for(rows, "sysio.token"_n);
   BOOST_REQUIRE(token_row);
   require_code_hash(*this, *token_row, "sysio.token"_n, contracts::token_wasm());
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(bios_records_abi_and_code_hashes) try {
   fc::temp_directory tempdir;
   validating_tester  t(tempdir, true);
   t.execute_setup_policy(setup_policy::full);
   const abi_serializer bios_ser(fc::json::from_string(contracts::bios_abi().data()).as<abi_def>(),
                                 base_tester::fixture_abi_yield_function());

   constexpr auto bios_tracked = "sysio.track"_n;
   t.create_accounts({bios_tracked});

   // sysio.bios recorded itself when it was deployed on sysio.
   const auto bios_row = row_for(table_rows(t, bios_ser, codehash_table, "code_hash"), config::system_account_name);
   BOOST_REQUIRE(bios_row);
   require_code_hash(t, *bios_row, config::system_account_name, contracts::bios_wasm());

   t.set_code(bios_tracked, contracts::token_wasm());
   auto stamp = pending_stamp(t);
   auto row   = row_for(table_rows(t, bios_ser, codehash_table, "code_hash"), bios_tracked);
   BOOST_REQUIRE(row);
   require_code_hash(t, *row, bios_tracked, contracts::token_wasm());
   require_stamp(*row, stamp);

   t.set_abi(bios_tracked, contracts::token_abi().data());
   stamp = pending_stamp(t);
   row   = row_for(table_rows(t, bios_ser, abihash_table, "abi_hash"), bios_tracked);
   BOOST_REQUIRE(row);
   BOOST_REQUIRE_EQUAL((*row)["hash"].as<fc::sha256>(), packed_abi_hash(contracts::token_abi().data()));
   require_stamp(*row, stamp);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
