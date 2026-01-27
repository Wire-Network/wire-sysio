#include <sysio/chain/abi_serializer.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

struct genesis_account {
   account_name aname;
   uint64_t     initial_balance;
};

std::vector<genesis_account> test_genesis( {
  {"b1"_n,       100'000'000'0000ll},
  {"whale4"_n,    40'000'000'0000ll},
  {"whale3"_n,    30'000'000'0000ll},
  {"whale2"_n,    20'000'000'0000ll},
  {"proda"_n,      1'000'000'0000ll},
  {"prodb"_n,      1'000'000'0000ll},
  {"prodc"_n,      1'000'000'0000ll},
  {"prodd"_n,      1'000'000'0000ll},
  {"prode"_n,      1'000'000'0000ll},
  {"prodf"_n,      1'000'000'0000ll},
  {"prodg"_n,      1'000'000'0000ll},
  {"prodh"_n,      1'000'000'0000ll},
  {"prodi"_n,      1'000'000'0000ll},
  {"prodj"_n,      1'000'000'0000ll},
  {"prodk"_n,      1'000'000'0000ll},
  {"prodl"_n,      1'000'000'0000ll},
  {"prodm"_n,      1'000'000'0000ll},
  {"prodn"_n,      1'000'000'0000ll},
  {"prodo"_n,      1'000'000'0000ll},
  {"prodp"_n,      1'000'000'0000ll},
  {"prodq"_n,      1'000'000'0000ll},
  {"prodr"_n,      1'000'000'0000ll},
  {"prods"_n,      1'000'000'0000ll},
  {"prodt"_n,      1'000'000'0000ll},
  {"produ"_n,      1'000'000'0000ll},
  {"runnerup1"_n,  1'000'000'0000ll},
  {"runnerup2"_n,  1'000'000'0000ll},
  {"runnerup3"_n,  1'000'000'0000ll},
  {"minow1"_n,           100'0000ll},
  {"minow2"_n,             1'0000ll},
  {"minow3"_n,             1'0000ll},
  {"masses"_n,   800'000'000'0000ll}
});

class bootseq_tester : public validating_tester {
public:
   void deploy_contract( bool call_init = true ) {
      set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
      set_abi( config::system_account_name, test_contracts::sysio_system_abi() );
      if( call_init ) {
         base_tester::push_action(config::system_account_name, "init"_n,
                                  config::system_account_name,  mutable_variant_object()
                                  ("version", 0)
                                  ("core", symbol(CORE_SYMBOL).to_string())
            );
      }
      const auto* accnt = control->find_account_metadata( config::system_account_name );
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
   }

   fc::variant get_global_state() {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, "global"_n, "global"_n );
      if (data.empty()) std::cout << "\nData is empty\n" << std::endl;
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "sysio_global_state", data, abi_serializer::create_yield_function( abi_serializer_max_time ) );
   }

    void create_currency( name contract, name manager, asset maxsupply, const private_key_type* signer = nullptr ) {
        auto act =  mutable_variant_object()
                ("issuer",       manager )
                ("maximum_supply", maxsupply );

        base_tester::push_action(contract, "create"_n, contract, act );
    }

    auto issue( name contract, name manager, name to, asset amount ) {
       auto r = base_tester::push_action( contract, "issue"_n, manager, mutable_variant_object()
                ("to",      to )
                ("quantity", amount )
                ("memo", "")
        );
        produce_block();
        return r;
    }

    auto register_producer(name producer) {
       auto r = base_tester::push_action(config::system_account_name, "regproducer"_n, producer, mvo()
                       ("producer",  name(producer))
                       ("producer_key", get_public_key( producer, "active" ) )
                       ("url", "" )
                       ("location", 0 )
                    );
       produce_block();
       return r;
    }

    asset get_balance( const account_name& act ) {
         return get_currency_balance("sysio.token"_n, symbol(CORE_SYMBOL), act);
    }

    void set_code_abi(const account_name& account, const vector<uint8_t>& wasm, const std::string& abi, const private_key_type* signer = nullptr) {
       wlog("account {}", account);
        set_code(account, wasm, signer);
        set_abi(account, abi, signer);
        if (account == config::system_account_name) {
           const auto* accnt = control->find_account_metadata( account );
           BOOST_REQUIRE(accnt != nullptr);
           abi_def abi_definition;
           BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi_definition), true);
           abi_ser.set_abi(std::move(abi_definition), abi_serializer::create_yield_function( abi_serializer_max_time ));
        }
        produce_blocks();
    }


    abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(bootseq_tests)

BOOST_FIXTURE_TEST_CASE( bootseq_test, bootseq_tester ) {
    try {
        // Create sysio.msig and sysio.token
        create_accounts({"sysio.msig"_n, "sysio.token"_n });
        // Set code for the following accounts:
        //  - sysio (code: sysio.bios) (already set by tester constructor)
        //  - sysio.msig (code: sysio.msig)
        //  - sysio.token (code: sysio.token)

        set_code_abi("sysio.msig"_n,
                     test_contracts::sysio_msig_wasm(),
                     test_contracts::sysio_msig_abi());//, &sysio_active_pk);
        // deployed as part of tester setup
        // set_code_abi("sysio.roa"_n,
        //              contracts::sysio_roa_wasm(),
        //              contracts::sysio_roa_abi());//, &sysio_active_pk);
        set_code_abi("sysio.token"_n,
                     test_contracts::sysio_token_wasm(),
                     test_contracts::sysio_token_abi()); //, &sysio_active_pk);

        // Set privileged for sysio.msig and sysio.token
        set_privileged("sysio.msig"_n);
        set_privileged("sysio.roa"_n);
        set_privileged("sysio.token"_n);

        // Verify sysio.msig and sysio.token is privileged
        const auto* sysio_msig_acc = control->find_account_metadata("sysio.msig"_n);
        BOOST_REQUIRE(sysio_msig_acc != nullptr);
        BOOST_TEST(sysio_msig_acc->is_privileged() == true);
        const auto* sysio_roa_acc = control->find_account_metadata("sysio.roa"_n);
        BOOST_REQUIRE(sysio_roa_acc != nullptr);
        BOOST_TEST(sysio_roa_acc->is_privileged() == true);
        const auto* sysio_token_acc = control->find_account_metadata("sysio.token"_n);
        BOOST_REQUIRE(sysio_token_acc != nullptr);
        BOOST_TEST(sysio_token_acc->is_privileged() == true);

        // Create SYS tokens in sysio.token, set its manager as sysio
        auto max_supply = core_from_string("10000000000.0000"); /// 1x larger than 1B initial tokens
        auto initial_supply = core_from_string("1000000000.0000"); /// 1x larger than 1B initial tokens
        create_currency("sysio.token"_n, config::system_account_name, max_supply);
        // Issue the genesis supply of 1 billion SYS tokens to sysio.system
        issue("sysio.token"_n, config::system_account_name, config::system_account_name, initial_supply);

        auto actual = get_balance(config::system_account_name);
        BOOST_REQUIRE_EQUAL(initial_supply, actual);

        // Create genesis accounts
        for( const auto& a : test_genesis ) {
           create_account( a.aname, config::system_account_name );
        }

        deploy_contract();

        auto producer_candidates = {
                "proda"_n, "prodb"_n, "prodc"_n, "prodd"_n, "prode"_n, "prodf"_n, "prodg"_n,
                "prodh"_n, "prodi"_n, "prodj"_n, "prodk"_n, "prodl"_n, "prodm"_n, "prodn"_n,
                "prodo"_n, "prodp"_n, "prodq"_n, "prodr"_n, "prods"_n, "prodt"_n, "produ"_n,
                "runnerup1"_n, "runnerup2"_n, "runnerup3"_n
        };

        // Register producers
        for( auto pro : producer_candidates ) {
           register_producer(pro);
        }

        std::vector<name> producers = producer_candidates;
        producers.resize(21);
        set_producers(producers);

        produce_blocks(2); // sysio is producing by itself
        auto active_schedule = control->head_active_producers();
        BOOST_TEST(active_schedule.producers.size() == 1u);
        BOOST_TEST(active_schedule.producers.front().producer_name == name("sysio"));
        produce_block();
        // finishes round and does a complete another round before switching
        for (size_t i = 0; i < 24; ++i) {
           produce_block(); // switching to the new set
           active_schedule = control->head_active_producers();
           if (active_schedule.producers.size() > 1u)
              break;
        }

        BOOST_TEST_REQUIRE(active_schedule.producers.size() == 21);
        BOOST_TEST(active_schedule.producers.at( 0).producer_name == name("proda"));
        BOOST_TEST(active_schedule.producers.at( 1).producer_name == name("prodb"));
        BOOST_TEST(active_schedule.producers.at( 2).producer_name == name("prodc"));
        BOOST_TEST(active_schedule.producers.at( 3).producer_name == name("prodd"));
        BOOST_TEST(active_schedule.producers.at( 4).producer_name == name("prode"));
        BOOST_TEST(active_schedule.producers.at( 5).producer_name == name("prodf"));
        BOOST_TEST(active_schedule.producers.at( 6).producer_name == name("prodg"));
        BOOST_TEST(active_schedule.producers.at( 7).producer_name == name("prodh"));
        BOOST_TEST(active_schedule.producers.at( 8).producer_name == name("prodi"));
        BOOST_TEST(active_schedule.producers.at( 9).producer_name == name("prodj"));
        BOOST_TEST(active_schedule.producers.at(10).producer_name == name("prodk"));
        BOOST_TEST(active_schedule.producers.at(11).producer_name == name("prodl"));
        BOOST_TEST(active_schedule.producers.at(12).producer_name == name("prodm"));
        BOOST_TEST(active_schedule.producers.at(13).producer_name == name("prodn"));
        BOOST_TEST(active_schedule.producers.at(14).producer_name == name("prodo"));
        BOOST_TEST(active_schedule.producers.at(15).producer_name == name("prodp"));
        BOOST_TEST(active_schedule.producers.at(16).producer_name == name("prodq"));
        BOOST_TEST(active_schedule.producers.at(17).producer_name == name("prodr"));
        BOOST_TEST(active_schedule.producers.at(18).producer_name == name("prods"));
        BOOST_TEST(active_schedule.producers.at(19).producer_name == name("prodt"));
        BOOST_TEST(active_schedule.producers.at(20).producer_name == name("produ"));

// TODO: Complete this test
    } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
