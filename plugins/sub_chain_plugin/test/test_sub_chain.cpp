#define BOOST_TEST_MODULE sub_chain
#include <boost/test/included/unit_test.hpp>

#include <sysio/sub_chain_plugin/sub_chain_plugin.hpp>

#include <sysio/testing/tester.hpp>

#include <contracts.hpp>

namespace {
    using namespace sysio;
    using namespace sysio::chain;
    using namespace sysio::testing;

    BOOST_AUTO_TEST_SUITE(sub_chain)

        BOOST_AUTO_TEST_CASE(sub_chain_sanity) {
            try {
                BOOST_CHECK_EQUAL(1, 1);
            } FC_LOG_AND_RETHROW()
        }

        BOOST_AUTO_TEST_CASE(sub_chain_init_without_config) {
            try {
                sub_chain_plugin sub_chain;
                variables_map options;

                BOOST_CHECK_EXCEPTION(sub_chain.initialize(options), fc::exception, [] (const fc::exception& e) {
                                      return e.to_detail_string().find("s-chain-contract") != std::string::npos;
                                      });
            } FC_LOG_AND_RETHROW()
        }

        auto test_options(int argc, const char *argv[]) {
            bpo::options_description option_desc;
            sub_chain_plugin plugin;
            plugin.set_program_options(option_desc, option_desc);
            bpo::parsed_options parsed = bpo::command_line_parser(argc, argv).options(option_desc).run();
            bpo::variables_map parsed_options;
            bpo::store(parsed, parsed_options);
            bpo::notify(parsed_options);
            return parsed_options;
        }

        BOOST_AUTO_TEST_CASE(sub_chain_init_with_config) {
            try {
                // init a new sub chain plugin
                sub_chain_plugin sub_chain;

                // setup the command line options
                const char *argv[] = {"sub_chain", "--s-chain-contract", "fizzbop", "--s-chain-actions", "fizz", "bop"};
                auto parsed_options = test_options(6, argv);

                BOOST_CHECK_EQUAL("fizzbop", parsed_options.at("s-chain-contract").as<std::string>());
                BOOST_CHECK_EQUAL(2u, parsed_options.at("s-chain-actions").as<std::vector<std::string>>().size());
                BOOST_CHECK_EQUAL("fizz", parsed_options.at("s-chain-actions").as<std::vector<std::string>>().at(0));
                BOOST_CHECK_EQUAL("bop", parsed_options.at("s-chain-actions").as<std::vector<std::string>>().at(1));

                // Now we should initialize without error
                sub_chain.initialize(parsed_options);

                BOOST_CHECK_EQUAL("fizzbop"_n, sub_chain.get_contract_name());
            } FC_LOG_AND_RETHROW()
        }


        BOOST_AUTO_TEST_CASE(bad_config) {
            try {
                const char *bad_argvs[][5] = {
                    {"sub_chain", "--s-chain-contract", "something", "--s-chain-actions", "areallylongactionname"},
                    {"sub_chain", "--s-chain-contract", "something", "--s-chain-actions", "bad,format"},
                    {"sub_chain", "--s-chain-contract", "something", "--s-chain-actions", "bad format"},
                    {"sub_chain", "--s-chain-contract", "something", "--s-chain-actions", "$%^&*mat"},
                };

                auto bad_options = std::vector<variables_map>();
                for (auto argv: bad_argvs) {
                    bad_options.push_back(test_options(5, argv));
                }

                for (auto options: bad_options) {
                    sub_chain_plugin sub_chain;
                    wlog(options.at("s-chain-actions").as<std::vector<std::string>>().at(0).c_str());
                    BOOST_CHECK_EXCEPTION(sub_chain.initialize(options), fc::exception, [] (const fc::exception& e) {
                                          return e.to_detail_string().find("Invalid name") != std::string::npos;
                                          });
                }

                sub_chain_plugin plugin;
                const char *good_and_bad[] = {
                    "sub_chain", "--s-chain-contract", "something", "--s-chain-actions", "good", "b,a,d"
                };
                BOOST_CHECK_EXCEPTION(plugin.initialize(test_options(6, good_and_bad)), fc::exception,
                                      [] (const fc::exception& e) {
                                      return e.to_detail_string().find("Invalid name") != std::string::npos;
                                      });

                sub_chain_plugin plugin2;
                const char *many_args[] = {
                    "sub_chain", "--s-chain-contract", "something", "--s-chain-actions", "good", "better",
                    "--s-chain-actions", "brok en", "--s-chain-actions", "best"
                };
                BOOST_CHECK_EXCEPTION(plugin2.initialize(test_options(10, many_args)), fc::exception,
                                      [] (const fc::exception& e) {
                                      return e.to_detail_string().find("Invalid name") != std::string::npos;
                                      });
            } FC_LOG_AND_RETHROW()
        }


        BOOST_AUTO_TEST_CASE(nothing_to_report) {
            try {
                tester chain;
                chain.create_account("abbie"_n);
                chain.produce_block();

                sub_chain_plugin sub_chain;
                const char *argv[] = {"sub_chain", "--s-chain-contract", "abbie", "--s-chain-actions", "dance"};
                sub_chain.initialize(test_options(5, argv));

                chain.produce_block();
                chain.produce_block();

                // Verify no transactions found when there are none to find
                BOOST_CHECK_EQUAL(0u, sub_chain.find_relevant_transactions(*chain.control.get()).size());

                // Verify default prev_s_id is empty / zero
                BOOST_CHECK_EQUAL(checksum256_type(), sub_chain.get_prev_s_id());
            } FC_LOG_AND_RETHROW()
        }

        BOOST_AUTO_TEST_CASE(find_relevant_txns) {
            try {
                tester chain;
                auto abbie = "abbie"_n;
                auto darcy = "darcy"_n;
                chain.create_account(abbie);
                chain.register_node_owner(abbie, 1);
                chain.set_code(abbie, contracts::dancer_wasm());
                chain.set_abi(abbie, contracts::dancer_abi());
                chain.create_account(darcy);
                chain.register_node_owner(darcy, 1);
                chain.set_code(darcy, contracts::dancer_wasm());
                chain.set_abi(darcy, contracts::dancer_abi());
                chain.produce_block();

                // setup sub chain to watch abbie dance
                sub_chain_plugin sub_chain;
                const char *argv[] = {"sub_chain", "--s-chain-contract", "abbie", "--s-chain-actions", "dance"};
                sub_chain.initialize(test_options(5, argv));

                // Verify no transactions found when there are none to find
                BOOST_CHECK_EQUAL(0u, sub_chain.find_relevant_transactions(*chain.control.get()).size());

                // Create a transaction that should be found
                chain.push_action(abbie, "dance"_n, abbie, mutable_variant_object());
                // Create a transaction that should be ignored
                chain.push_action(darcy, "dance"_n, darcy, mutable_variant_object());

                // sub chain should find the transaction in the current queue
                BOOST_CHECK_EQUAL(1u, sub_chain.find_relevant_transactions(*chain.control.get()).size());
                BOOST_CHECK_EQUAL(checksum256_type(), sub_chain.get_prev_s_id());

                chain.produce_block();
                // once the block is produced, sub chain no longer sees the transaction
                BOOST_CHECK_EQUAL(0u, sub_chain.find_relevant_transactions(*chain.control.get()).size());
            } FC_LOG_AND_RETHROW()
        }

        void producer_api_flow(tester *chain, sub_chain_plugin *sub_chain) {
            // Step 1: Get the relevant transactions
            const auto &relevant_s_transactions = sub_chain->find_relevant_transactions(*chain->control.get());
            if (relevant_s_transactions.empty()) {
                return;
            }
            // Step 2: Calculate the S-Root
            checksum256_type s_root = sub_chain->calculate_s_root(relevant_s_transactions);
            // Step 4: S-Root is hashed with the previous S-ID using SHA-256
            // Step 5: Takes the 32 least-significant bits from the previous S-ID to get the previous S-Block number, and increment to get the new S-Block number
            // Step 6: Hashes the S-Root with the previous S-ID with SHA-256, then replace the 32 least significant bits with the new S-Block number to produce the new S-ID
            checksum256_type curr_s_id = sub_chain->compute_curr_s_id(s_root);
            // Prepare the s_header for the current block to be added to the header extension
            s_header s_header{
                sub_chain->get_contract_name(),
                sub_chain->get_prev_s_id(),
                curr_s_id,
                s_root
            };
            // Set the s_root in the chain controller for the building block state
            auto &controller = *chain->control.get();
//            controller.set_s_header(s_header);
            sub_chain->update_prev_s_id(curr_s_id);
        }

        BOOST_AUTO_TEST_CASE(track_s_id) {
            try {
                tester chain;

                auto candice = "candice"_n; // a tier 1 node owner
                auto fiona = "fiona"_n; // a normal user with a policy grant from candice
                chain.create_accounts({candice, fiona});
                chain.register_node_owner(candice, 1);
                chain.add_roa_policy(candice, fiona,
                  "10.0000 SYS", "10.0000 SYS", "10.0000 SYS",
                  0, chain.control->last_irreversible_block_num()
                );
                chain.produce_block();

                // Load both accounts with the dancer test contract
                chain.set_code(candice, contracts::dancer_wasm());
                chain.set_abi(candice, contracts::dancer_abi());
                chain.register_node_owner(fiona, 1);
                chain.set_code(fiona, contracts::dancer_wasm());
                chain.set_abi(fiona, contracts::dancer_abi());
                chain.produce_block();

                // Configure sub_chain plugin to _only_ track fiona's dancing
                sub_chain_plugin sub_chain;
                const char *argv[] = {"sub_chain", "--s-chain-contract", "fiona", "--s-chain-actions", "dance"};
                sub_chain.initialize(test_options(5, argv));

                producer_api_flow(&chain, &sub_chain);

                // Current s_id should still be zero
                BOOST_CHECK_EQUAL(checksum256_type(), sub_chain.get_prev_s_id());
                chain.produce_block();
                BOOST_CHECK_EQUAL(checksum256_type(), sub_chain.get_prev_s_id());

                // Push ignored transaction
                chain.push_action(candice, "dance"_n, candice, mutable_variant_object());
                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();
                BOOST_CHECK_EQUAL(checksum256_type(), sub_chain.get_prev_s_id());

                // now push a tracked transaction and sroot should update
                chain.push_action(fiona, "dance"_n, fiona, mutable_variant_object());
                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();
                auto last_s_id = sub_chain.get_prev_s_id();
                BOOST_CHECK_NE(checksum256_type(), sub_chain.get_prev_s_id());

                // no transactions, sroot should stay the same
                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();
                BOOST_CHECK_EQUAL(last_s_id, sub_chain.get_prev_s_id());

                // push a few more transactions and sroot should update
                chain.push_action(candice, "dance"_n, candice, mutable_variant_object());
                chain.push_action(fiona, "dance"_n, fiona, mutable_variant_object());

                const string expected_s_id_str = "000000027106881deac4f2ab75ea922aec49793f792c5aa5a5659165e6a9ed8f";

                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();
                BOOST_CHECK_NE(last_s_id, sub_chain.get_prev_s_id());
                BOOST_CHECK_EQUAL(expected_s_id_str, sub_chain.get_prev_s_id().str());

                // push a non-tracked action on the tracked contract
                chain.push_action(fiona, "stop"_n, fiona, mutable_variant_object());
                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();

                auto gina = "gina"_n; // normal user without a policy grant
                chain.create_account(gina);

                chain.push_action(candice, "stop"_n, gina, mutable_variant_object());
                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();

                // sroot should not change
                BOOST_CHECK_EQUAL(expected_s_id_str, sub_chain.get_prev_s_id().str());

                chain.push_action(fiona, "dance"_n, gina, mutable_variant_object());
                producer_api_flow(&chain, &sub_chain);
                chain.produce_block();

                BOOST_CHECK_NE(expected_s_id_str, sub_chain.get_prev_s_id().str());

            } FC_LOG_AND_RETHROW()
        }

    BOOST_AUTO_TEST_SUITE_END()
} // namespace
