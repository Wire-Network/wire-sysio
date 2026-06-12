#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/unapplied_transaction_queue.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/merkle.hpp>
#include <sysio/chain/s_root_extension.hpp>
#include <fc/variant_object.hpp>

#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

using mvo = fc::mutable_variant_object;

BOOST_AUTO_TEST_SUITE(test_s_root_extension)

BOOST_AUTO_TEST_CASE(nothing_there) {
   try {
      tester chain;
      chain.produce_block();
      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block1 = chain.produce_block();

      BOOST_CHECK_EQUAL(0u, block1->header_extensions.size()); // no header extensions (finality fields are now direct members)
   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE(nothing_to_report) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
      matches[0].add_action("batchw"_n, contract_action_match::match_type::exact);
      tester chain(matches);
      chain.produce_block();

      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block1 = chain.produce_block();

      BOOST_CHECK_EQUAL(0u, block1->header_extensions.size()); // no header extensions (finality fields are now direct members)
   } FC_LOG_AND_RETHROW()
}


BOOST_AUTO_TEST_CASE(something_to_report) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
      matches[0].add_action("newaccount"_n, contract_action_match::match_type::exact);
      tester chain(matches);
      chain.produce_block();
      chain.create_account("brian"_n);
      auto brian_block = chain.produce_block();
      auto find_s_root_ext = [](const auto& exts) {
         return std::find_if(exts.begin(), exts.end(),
            [](const auto& ext) { return ext.first == s_root_extension::extension_id(); });
      };
      auto count_s_root_ext = [](const auto& block) -> size_t {
         flat_multimap<uint16_t, block_header_extension> header_exts = block->validate_and_extract_header_extensions();
         return header_exts.count(s_root_extension::extension_id());
      };
      checksum256_type previous_s_root;
      block_num_type previous_block_num = 0;
      for (size_t i = 1; i < brian_block->block_num(); ++i) {
         auto b = chain.control->fetch_block_by_number(i);
         if (count_s_root_ext(b) > 0) {
            previous_s_root = b->extract_header_extension<s_root_extension>().s_header_data.current_s_id;
            previous_block_num = b->block_num();
         }
      }

      auto block = chain.control->fetch_block_by_number(brian_block->block_num() - 1);
      BOOST_CHECK_EQUAL(0u, count_s_root_ext(block));

      block = chain.control->fetch_block_by_number(brian_block->block_num());
      flat_multimap<uint16_t, block_header_extension> header_exts = block->validate_and_extract_header_extensions();
      BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

      auto crtd_it = find_s_root_ext(header_exts);

      BOOST_CHECK(crtd_it != header_exts.end());
      s_root_extension crtd_s_ext_brian = std::get<s_root_extension>(crtd_it->second);
      s_header crtd_s_header_brian = crtd_s_ext_brian.s_header_data;
      BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header_brian.contract_name);
      BOOST_CHECK_EQUAL(previous_s_root, crtd_s_header_brian.previous_s_id);
      BOOST_CHECK_EQUAL(previous_block_num, crtd_s_header_brian.previous_block_num);
      BOOST_CHECK(checksum256_type() != crtd_s_header_brian.current_s_id);
      BOOST_CHECK(checksum256_type() != crtd_s_header_brian.current_s_root);

      chain.produce_block();
      auto block_after = chain.control->fetch_block_by_number(brian_block->block_num() + 1);
      BOOST_CHECK_EQUAL(0u, count_s_root_ext(block_after));

      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block_with_multiple = chain.produce_block();

      BOOST_CHECK_EQUAL(1u, count_s_root_ext(block_with_multiple));
      header_exts = block_with_multiple->validate_and_extract_header_extensions();
      BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

      crtd_it = find_s_root_ext(header_exts);

      BOOST_CHECK(crtd_it != header_exts.end());
      s_root_extension crtd_s_ext_multiple = std::get<s_root_extension>(crtd_it->second);
      s_header crtd_s_header_multiple = crtd_s_ext_multiple.s_header_data;
      BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header_multiple.contract_name);
      BOOST_CHECK_EQUAL(crtd_s_header_brian.current_s_id, crtd_s_header_multiple.previous_s_id);
      BOOST_CHECK_EQUAL(brian_block->block_num(), crtd_s_header_multiple.previous_block_num);
      BOOST_CHECK(checksum256_type() != crtd_s_header_multiple.current_s_id);
      BOOST_CHECK(checksum256_type() != crtd_s_header_multiple.current_s_root);
   } FC_LOG_AND_RETHROW()
}

namespace {

// Extract the s_header from a block that must carry exactly one s_root_extension.
s_header extract_single_s_header(const signed_block_ptr& block) {
   flat_multimap<uint16_t, block_header_extension> header_exts = block->validate_and_extract_header_extensions();
   BOOST_REQUIRE_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));
   auto itr = std::find_if(header_exts.begin(), header_exts.end(),
      [](const auto& ext) { return ext.first == s_root_extension::extension_id(); });
   BOOST_REQUIRE(itr != header_exts.end());
   return std::get<s_root_extension>(itr->second).s_header_data;
}

} // namespace

// The S-root merkle commits to the set of transactions that touched a matched
// contract, so a transaction must contribute exactly one leaf per (contract, root)
// no matter how many of its action traces match. A transfer notifying `from` and
// `to` via require_recipient yields three matching traces (receiver = contract,
// from, to) that all share one transaction id; before the dedup fix each trace
// produced its own identical leaf.
BOOST_AUTO_TEST_CASE(notification_produces_single_leaf) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, "sysio.token"_n, contract_action_match::match_type::exact));
      matches[0].add_action("transfer"_n, contract_action_match::match_type::exact);
      tester chain(matches);

      chain.create_accounts({"sysio.token"_n, "alice"_n});
      chain.set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
      chain.set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
      // the token contract bills the currency stats row to the issuer account
      chain.set_privileged("sysio.token"_n);
      chain.produce_block();

      // create/issue do not match the configured (sysio.token, transfer) pair, and
      // issuing to the issuer avoids the contract's inline transfer on issue.
      chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mvo()
         ("issuer", name(config::system_account_name))
         ("maximum_supply", core_from_string("1000000.0000")));
      chain.push_action("sysio.token"_n, "issue"_n, config::system_account_name, mvo()
         ("to", name(config::system_account_name))
         ("quantity", core_from_string("1000.0000"))
         ("memo", ""));
      chain.produce_block();

      auto trace = chain.push_action("sysio.token"_n, "transfer"_n, config::system_account_name, mvo()
         ("from", name(config::system_account_name))
         ("to", "alice"_n)
         ("quantity", core_from_string("1.0000"))
         ("memo", ""));
      auto block = chain.produce_block();

      const s_header header = extract_single_s_header(block);
      BOOST_CHECK_EQUAL("sysio.token"_n, header.contract_name);
      const deque<digest_type> expected_leaves{trace->id};
      BOOST_CHECK_EQUAL(calculate_merkle(expected_leaves), header.current_s_root);
   } FC_LOG_AND_RETHROW()
}

// Two matching actions in one transaction must also collapse to a single leaf.
BOOST_AUTO_TEST_CASE(repeated_matching_actions_produce_single_leaf) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
      matches[0].add_action("newaccount"_n, contract_action_match::match_type::exact);
      tester chain(matches);
      chain.produce_block();

      // sysio.-prefixed accounts skip the tester's setalimits/ROA companion actions,
      // leaving a transaction with exactly two matching newaccount actions.
      signed_transaction trx;
      chain.set_transaction_headers(trx);
      for (const auto& account : {"sysio.aaa"_n, "sysio.bbb"_n}) {
         trx.actions.emplace_back(vector<permission_level>{{config::system_account_name, config::active_name}},
                                  newaccount{
                                     .creator = config::system_account_name,
                                     .name    = account,
                                     .owner   = authority(chain.get_public_key(account, "owner")),
                                     .active  = authority(chain.get_public_key(account, "active"))
                                  });
      }
      chain.set_transaction_headers(trx);
      trx.sign(chain.get_private_key(config::system_account_name, "active"), chain.control->get_chain_id());
      auto trace = chain.push_transaction(trx);
      auto block = chain.produce_block();

      const s_header header = extract_single_s_header(block);
      BOOST_CHECK_EQUAL(config::system_account_name, header.contract_name);
      const deque<digest_type> expected_leaves{trace->id};
      BOOST_CHECK_EQUAL(calculate_merkle(expected_leaves), header.current_s_root);
   } FC_LOG_AND_RETHROW()
}

// Distinct transactions must keep one leaf each, in application order — the
// dedup must only collapse traces of the same transaction, never across
// transactions.
BOOST_AUTO_TEST_CASE(distinct_transactions_produce_one_leaf_each) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
      matches[0].add_action("newaccount"_n, contract_action_match::match_type::exact);
      tester chain(matches);
      chain.produce_block();

      auto trace1 = chain.create_account("carol"_n);
      auto trace2 = chain.create_account("dave"_n);
      auto block = chain.produce_block();

      const s_header header = extract_single_s_header(block);
      const deque<digest_type> expected_leaves{trace1->id, trace2->id};
      BOOST_CHECK_EQUAL(calculate_merkle(expected_leaves), header.current_s_root);
   } FC_LOG_AND_RETHROW()
}

// Direct unit tests for the validate_s_root_extensions_match helper extracted
// from apply_block. Each test builds received/constructed extension vectors by
// hand and verifies the helper accepts or rejects as expected.
namespace {

// Build a packed s_root_extension with distinct-but-valid fields. The `nonce`
// byte perturbs current_s_id so two entries with different nonces compare
// unequal after packing.
std::vector<char> make_sre(uint8_t nonce) {
   s_header h;
   h.contract_name = "sysio"_n;
   h.previous_s_id = checksum256_type();
   h.current_s_id  = checksum256_type();
   h.current_s_id._hash[0] = nonce; // differentiate payloads
   h.current_s_root = checksum256_type();
   h.previous_block_num = 0;
   return fc::raw::pack(s_root_extension(h));
}

// Append an s_root_extension entry to an extensions_type.
void push_sre(extensions_type& v, uint8_t nonce) {
   v.emplace_back(s_root_extension::extension_id(), make_sre(nonce));
}

// Append a non-s_root extension entry (extension_id = 0) with arbitrary bytes.
// Used to verify the helper filters by extension_id rather than just position.
void push_other(extensions_type& v, const std::string& s) {
   v.emplace_back(uint16_t{0}, std::vector<char>(s.begin(), s.end()));
}

} // namespace

BOOST_AUTO_TEST_CASE(validate_match_zero_entries) {
   extensions_type received;
   extensions_type constructed;
   BOOST_CHECK_NO_THROW(validate_s_root_extensions_match(received, constructed));
}

BOOST_AUTO_TEST_CASE(validate_match_zero_among_other_extensions) {
   // No s_root_extensions on either side, but both have unrelated extensions.
   // Helper must filter by extension_id and accept.
   extensions_type received;    push_other(received,    "pfa_bytes");
   extensions_type constructed; push_other(constructed, "pfa_bytes_different"); // different content - ignored
   BOOST_CHECK_NO_THROW(validate_s_root_extensions_match(received, constructed));
}

BOOST_AUTO_TEST_CASE(validate_match_one_entry) {
   extensions_type received;    push_sre(received,    1);
   extensions_type constructed; push_sre(constructed, 1);
   BOOST_CHECK_NO_THROW(validate_s_root_extensions_match(received, constructed));
}

BOOST_AUTO_TEST_CASE(validate_match_three_entries_in_order) {
   extensions_type received;
   push_sre(received, 1); push_sre(received, 2); push_sre(received, 3);
   extensions_type constructed;
   push_sre(constructed, 1); push_sre(constructed, 2); push_sre(constructed, 3);
   BOOST_CHECK_NO_THROW(validate_s_root_extensions_match(received, constructed));
}

BOOST_AUTO_TEST_CASE(validate_match_three_entries_with_interleaved_other) {
   // s_root_extensions interleaved with other extension types. The helper only
   // compares s_root_extensions by position-within-sre, ignoring everything else.
   extensions_type received;
   push_other(received, "x");
   push_sre(received, 1);
   push_other(received, "y");
   push_sre(received, 2);
   push_sre(received, 3);
   extensions_type constructed;
   push_sre(constructed, 1);
   push_other(constructed, "z"); // different position of "other" - should not matter
   push_sre(constructed, 2);
   push_sre(constructed, 3);
   BOOST_CHECK_NO_THROW(validate_s_root_extensions_match(received, constructed));
}

BOOST_AUTO_TEST_CASE(validate_rejects_received_has_more) {
   extensions_type received;
   push_sre(received, 1); push_sre(received, 2);
   extensions_type constructed;
   push_sre(constructed, 1);
   BOOST_CHECK_EXCEPTION(validate_s_root_extensions_match(received, constructed),
      block_validate_exception,
      fc_exception_message_contains("count mismatch"));
}

BOOST_AUTO_TEST_CASE(validate_rejects_constructed_has_more) {
   extensions_type received;
   push_sre(received, 1);
   extensions_type constructed;
   push_sre(constructed, 1); push_sre(constructed, 2);
   BOOST_CHECK_EXCEPTION(validate_s_root_extensions_match(received, constructed),
      block_validate_exception,
      fc_exception_message_contains("count mismatch"));
}

BOOST_AUTO_TEST_CASE(validate_rejects_only_one_side_has_sre) {
   extensions_type received;
   push_sre(received, 1);
   extensions_type constructed; // empty
   BOOST_CHECK_EXCEPTION(validate_s_root_extensions_match(received, constructed),
      block_validate_exception,
      fc_exception_message_contains("count mismatch"));
}

BOOST_AUTO_TEST_CASE(validate_rejects_payload_mismatch_at_first_slot) {
   extensions_type received;    push_sre(received,    1);
   extensions_type constructed; push_sre(constructed, 2);
   BOOST_CHECK_EXCEPTION(validate_s_root_extensions_match(received, constructed),
      block_validate_exception,
      fc_exception_message_contains("payload mismatch at slot 1"));
}

BOOST_AUTO_TEST_CASE(validate_rejects_payload_mismatch_at_second_slot) {
   extensions_type received;
   push_sre(received, 1); push_sre(received, 2);
   extensions_type constructed;
   push_sre(constructed, 1); push_sre(constructed, 99);
   BOOST_CHECK_EXCEPTION(validate_s_root_extensions_match(received, constructed),
      block_validate_exception,
      fc_exception_message_contains("payload mismatch at slot 2"));
}

BOOST_AUTO_TEST_SUITE_END()
