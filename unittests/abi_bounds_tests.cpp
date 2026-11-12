#include <fc/io/json.hpp>
#include <sysio/chain/abi_bounds.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <filesystem>

using namespace sysio::chain;

namespace {
constexpr uint32_t forged_count = 1024 * 1024;
constexpr uint32_t excessive_type_count = 65536;

/// Submit packed bytes directly so the regression exercises native setabi without client-side ABI validation.
void publish_abi(sysio::testing::tester& chain, name account, const bytes& packed) {
   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{account, config::active_name}},
                            setabi{.account = account, .abi = packed});
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(account, "active"), chain.control->get_chain_id());
   chain.push_transaction(trx);
}
} // namespace

BOOST_AUTO_TEST_SUITE(abi_bounds_tests)

/// The bounds checker rejects forged lengths without constructing the declared strings or containers.
BOOST_AUTO_TEST_CASE(forged_lengths_and_counts_are_rejected) {
   const auto tiny = fc::raw::pack(std::string{"sysio::abi/1.2"}, fc::unsigned_int{forged_count});
   BOOST_REQUIRE_EQUAL(tiny.size(), 18u);
   BOOST_CHECK_THROW(check_abi_bounds(tiny.data(), tiny.size()), abi_exception);
   const auto string = fc::raw::pack(fc::unsigned_int{forged_count}, uint8_t{0});
   BOOST_CHECK_THROW(check_abi_bounds(string.data(), string.size()), abi_exception);
   const auto nested = fc::raw::pack(std::string{"sysio::abi/1.2"}, fc::unsigned_int{0},
      fc::unsigned_int{1}, std::string{"entry"}, std::string{}, fc::unsigned_int{forged_count});
   BOOST_CHECK_THROW(check_abi_bounds(nested.data(), nested.size()), abi_exception);

   // A count fitting the remaining byte count can still describe truncated multi-field entries.
   auto padded = fc::raw::pack(std::string{"sysio::abi/1.2"}, fc::unsigned_int{1024});
   padded.resize(padded.size() + 1024);
   BOOST_CHECK_THROW(check_abi_bounds(padded.data(), padded.size()), abi_exception);
}

/// Reject truncated, overflowing, and unterminated varints while preserving non-minimal uint32 encodings.
BOOST_AUTO_TEST_CASE(length_varints_are_bounded) {
   for (const bytes& packed : {bytes{char(0x80)},
      bytes{char(0xff), char(0xff), char(0xff), char(0xff), char(0x10)},
      bytes{char(0x80), char(0x80), char(0x80), char(0x80), char(0x80), 0},
      bytes{0, 0, 0, char(0x80)}}) {
      BOOST_CHECK_THROW(check_abi_bounds(packed.data(), packed.size()), fc::exception);
   }
   auto packed = fc::raw::pack(abi_def{});
   packed.front() = char(0x80);
   packed.insert(packed.begin() + 1, 0);
   BOOST_CHECK_NO_THROW(check_abi_bounds(packed.data(), packed.size()));
}

/// Enforce the packed-size boundary and reject fully encoded definitions that exceed decoded-storage bounds.
BOOST_AUTO_TEST_CASE(resource_limits_are_deterministic) {
   abi_def abi;
   abi.protobuf_types.value.resize(abi_limits::max_packed_bytes, 'x');
   auto packed = fc::raw::pack(abi);
   abi.protobuf_types.value.resize(abi.protobuf_types.value.size() - (packed.size() - abi_limits::max_packed_bytes));
   packed = fc::raw::pack(abi);
   BOOST_REQUIRE_EQUAL(packed.size(), abi_limits::max_packed_bytes);
   BOOST_CHECK_NO_THROW(check_abi_bounds(packed.data(), packed.size()));
   packed.push_back(0);
   BOOST_CHECK_THROW(check_abi_bounds(packed.data(), packed.size()), abi_exception);

   abi = {};
   abi.types.resize(excessive_type_count);
   packed = fc::raw::pack(abi);
   BOOST_REQUIRE_LT(packed.size(), abi_limits::max_packed_bytes);
   BOOST_CHECK_EXCEPTION(check_abi_bounds(packed.data(), packed.size()), abi_exception,
      [](const abi_exception& error) {
         return error.to_detail_string().find("decoded storage limit") != std::string::npos;
      });
}

/// Accept optional tails, extensions, enums, and opaque protobuf metadata without semantic interpretation.
BOOST_AUTO_TEST_CASE(supported_optional_fields_are_accepted) {
   abi_def abi;
   abi.version = "sysio::abi/1.2";
   abi.abi_extensions.emplace_back(7, bytes{'x', 'y'});
   auto packed = fc::raw::pack(std::tie(abi.version, abi.types, abi.structs, abi.actions, abi.tables,
      abi.ricardian_clauses, abi.error_messages, abi.abi_extensions));
   BOOST_CHECK_NO_THROW(check_abi_bounds(packed.data(), packed.size()));
   const auto append = [&](const auto& field) {
      const auto tail = fc::raw::pack(field);
      packed.insert(packed.end(), tail.begin(), tail.end());
      BOOST_CHECK_NO_THROW(check_abi_bounds(packed.data(), packed.size()));
   };
   abi.variants.value.push_back({"choice", {"uint64", "string"}});
   append(abi.variants);
   abi.action_results.value.emplace_back("result"_n, "uint64");
   append(abi.action_results);
   abi.enums.value.push_back({"status", "uint8", {{"ok", 0}, {"error", 1}}});
   append(abi.enums);
   abi.protobuf_types.value = R"({"file":[]})";
   append(abi.protobuf_types);
   packed.push_back('x'); // Preserve the existing opaque-tail behavior for future fields.
   BOOST_CHECK_NO_THROW(check_abi_bounds(packed.data(), packed.size()));
}

/// Every production contract ABI passes publication bounds and retains its normal packed representation.
BOOST_AUTO_TEST_CASE(production_contract_abis_are_accepted) {
   const auto contracts = std::filesystem::path(__FILE__).parent_path().parent_path() / "contracts";
   size_t checked = 0;
   for (const auto& entry : std::filesystem::recursive_directory_iterator(contracts)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".abi")
         continue;
      BOOST_TEST_CONTEXT(entry.path().string()) {
         const auto abi = fc::json::from_file(entry.path()).as<abi_def>();
         const auto packed = fc::raw::pack(abi);
         BOOST_CHECK_NO_THROW(check_abi_bounds(packed.data(), packed.size()));
         BOOST_CHECK(fc::raw::pack(fc::raw::unpack<abi_def>(packed)) == packed);
         ++checked;
      }
   }
   BOOST_CHECK_GT(checked, 0u);
}

/// Native publication rejects unsafe packed structure atomically but accepts bounded semantic errors and clearing.
BOOST_AUTO_TEST_CASE(setabi_checks_bounds_without_semantic_validation) {
   sysio::testing::tester chain;
   const auto account = "abibounds"_n;
   chain.create_account(account);
   abi_def abi;
   abi.version = "sysio::abi/1.2";
   abi.types.emplace_back("alias", "missing_type");
   const auto packed = fc::raw::pack(abi);
   BOOST_CHECK_THROW(abi_serializer(abi, abi_serializer::create_depth_yield_function()), invalid_type_inside_abi);
   BOOST_REQUIRE_NO_THROW(publish_abi(chain, account, packed));
   const auto* metadata = chain.control->find_account_metadata(account);
   BOOST_REQUIRE(metadata != nullptr);
   BOOST_CHECK_GT(metadata->abi.size(), 0u);
   const auto sequence = metadata->abi_sequence;
   BOOST_CHECK(fc::raw::pack(metadata->get_abi()) == packed);
   const auto forged = fc::raw::pack(std::string{"sysio::abi/1.2"}, fc::unsigned_int{forged_count});
   BOOST_CHECK_THROW(publish_abi(chain, account, forged), fc::exception);
   BOOST_CHECK_EQUAL(metadata->abi_sequence, sequence);
   BOOST_CHECK(fc::raw::pack(metadata->get_abi()) == packed);
   BOOST_CHECK_THROW(publish_abi(chain, account, bytes{char(0x80)}), fc::exception);
   BOOST_REQUIRE_NO_THROW(publish_abi(chain, account, {}));
   BOOST_CHECK_EQUAL(metadata->abi.size(), 0u);
   BOOST_CHECK_EQUAL(metadata->abi_sequence, sequence + 1);
   BOOST_CHECK_THROW(metadata->get_abi(), abi_not_found_exception);
}

BOOST_AUTO_TEST_SUITE_END()
