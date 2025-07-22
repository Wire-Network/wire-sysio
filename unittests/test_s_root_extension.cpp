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
      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block1 = chain.produce_block();

      BOOST_CHECK_EQUAL(0u, block1->header_extensions.size());
   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE(nothing_to_report) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
      matches[0].add_action("batchw"_n, contract_action_match::match_type::exact);
      tester chain(matches);

      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block1 = chain.produce_block();

      BOOST_CHECK_EQUAL(0u, block1->header_extensions.size());
   } FC_LOG_AND_RETHROW()
}


BOOST_AUTO_TEST_CASE(something_to_report) {
   try {
      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
      matches[0].add_action("newaccount"_n, contract_action_match::match_type::exact);
      tester chain(matches);
      auto find_s_root_ext = [](const auto& exts) {
         return std::find_if(exts.begin(), exts.end(),
            [](const auto& ext) { return ext.first == s_root_extension::extension_id(); });
      };
      for (uint i = 1; i < 4; ++i) {
         auto block = chain.control->fetch_block_by_number(i);
         BOOST_CHECK_EQUAL(i, block->block_num());
         flat_multimap<uint16_t, block_header_extension> header_exts = block->validate_and_extract_header_extensions();
         BOOST_CHECK_EQUAL(0u, header_exts.count(s_root_extension::extension_id()));
      }

      auto block4 = chain.control->fetch_block_by_number(4);
      BOOST_CHECK_EQUAL(4u, block4->block_num());
      BOOST_CHECK_EQUAL(2u, block4->header_extensions.size());

      flat_multimap<uint16_t, block_header_extension> header_exts = block4->validate_and_extract_header_extensions();
      BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

      auto crtd_it = find_s_root_ext(header_exts);

      BOOST_CHECK(crtd_it != header_exts.end());
      s_root_extension crtd_s_ext4 = std::get<s_root_extension>(crtd_it->second);
      s_header crtd_s_header4 = crtd_s_ext4.s_header_data;
      BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header4.contract_name);
      BOOST_CHECK_EQUAL(checksum256_type(), crtd_s_header4.previous_s_id);
      BOOST_CHECK_EQUAL(0u, crtd_s_header4.previous_block_num);
      BOOST_CHECK(checksum256_type() != crtd_s_header4.current_s_id);
      BOOST_CHECK(checksum256_type() != crtd_s_header4.current_s_root);

      auto block5 = chain.control->fetch_block_by_number(5);
      BOOST_CHECK_EQUAL(5u, block5->block_num());
      BOOST_CHECK_EQUAL(1u, block5->header_extensions.size());

      header_exts = block5->validate_and_extract_header_extensions();
      BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

      crtd_it = find_s_root_ext(header_exts);

      BOOST_CHECK(crtd_it != header_exts.end());
      s_root_extension crtd_s_ext5 = std::get<s_root_extension>(crtd_it->second);
      s_header crtd_s_header5 = crtd_s_ext5.s_header_data;
      BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header5.contract_name);
      BOOST_CHECK_EQUAL(crtd_s_header4.current_s_id, crtd_s_header5.previous_s_id);
      BOOST_CHECK_EQUAL(4u, crtd_s_header5.previous_block_num);
      BOOST_CHECK(checksum256_type() != crtd_s_header5.current_s_id);
      BOOST_CHECK(checksum256_type() != crtd_s_header5.current_s_root);

      chain.create_account("abbie"_n);
      chain.create_account("john"_n);
      chain.create_account("tommy"_n);
      auto block6 = chain.produce_block();

      BOOST_CHECK_EQUAL(6u, block6->block_num());
      BOOST_CHECK_EQUAL(1u, block6->header_extensions.size());
      header_exts = block6->validate_and_extract_header_extensions();
      BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

      crtd_it = find_s_root_ext(header_exts);

      BOOST_CHECK(crtd_it != header_exts.end());
      s_root_extension crtd_s_ext6 = std::get<s_root_extension>(crtd_it->second);
      s_header crtd_s_header6 = crtd_s_ext6.s_header_data;
      BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header6.contract_name);
      BOOST_CHECK_EQUAL(crtd_s_header5.current_s_id, crtd_s_header6.previous_s_id);
      BOOST_CHECK_EQUAL(5u, crtd_s_header6.previous_block_num);
      BOOST_CHECK(checksum256_type() != crtd_s_header6.current_s_id);
      BOOST_CHECK(checksum256_type() != crtd_s_header6.current_s_root);
   } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
