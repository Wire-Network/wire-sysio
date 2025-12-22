#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/unapplied_transaction_queue.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/s_root_extension.hpp>


using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(test_s_root_extension)

BOOST_AUTO_TEST_CASE(nothing_there) {
   try {
      tester chain;
      chain.produce_block();
      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block1 = chain.produce_block();

      BOOST_CHECK_EQUAL(1u, block1->header_extensions.size());
      BOOST_TEST(block1->contains_header_extension(finality_extension::extension_id())); // only finality_extension
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

      BOOST_CHECK_EQUAL(1u, block1->header_extensions.size());
      BOOST_TEST(block1->contains_header_extension(finality_extension::extension_id())); // only finality_extension
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

BOOST_AUTO_TEST_SUITE_END()
