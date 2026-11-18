#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/permission_link_object.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

#include "sysio_system_tester.hpp"

namespace {

using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;

/**
 * Validating tester with an account whose RAM quota can be set to leave an exact number of bytes free, and a
 * contract (`ram_restrictions_test`) that bills its rows to a named payer. Native `setcode`, `setabi` and
 * `updateauth` must leave the account within its quota at the moment they grow its RAM, so each test sizes the
 * quota around the change it makes.
 */
struct native_ram_quota_fixture : validating_tester {
   static constexpr account_name account     = "alice"_n;
   static constexpr account_name row_contract = "rambiller"_n;
   static constexpr permission_name spending  = "spending"_n;

   native_ram_quota_fixture() {
      create_accounts({account, row_contract});
      set_contract(row_contract, test_contracts::ram_restrictions_test_wasm(),
                   test_contracts::ram_restrictions_test_abi());
      produce_block();
   }

   int64_t ram_usage() const { return control->get_resource_limits_manager().get_account_ram_usage(account); }

   /// Sets the account's RAM quota to its current usage plus `headroom` bytes; CPU and NET stay unlimited.
   void set_ram_headroom(int64_t headroom) {
      push_action(config::system_account_name, "setalimits"_n, config::system_account_name,
                  mvo()("account", account)("ram_bytes", ram_usage() + headroom)("net_weight", -1)("cpu_weight", -1));
      produce_block();
   }

   /// Pushes `actions` as one transaction signed with the account's active key.
   transaction_trace_ptr push_actions(vector<action> actions) {
      signed_transaction trx;
      trx.actions = std::move(actions);
      set_transaction_headers(trx);
      trx.sign(get_private_key(account, "active"), control->get_chain_id());
      return push_transaction(trx);
   }

   static vector<permission_level> active_auth() { return {{account, config::active_name}}; }

   static action setcode_action(const vector<uint8_t>& wasm) {
      return action(active_auth(), setcode{.account = account, .code = bytes(wasm.begin(), wasm.end())});
   }

   static action setabi_action(const bytes& abi) { return action(active_auth(), setabi{.account = account, .abi = abi}); }

   static action updateauth_action(const authority& auth) {
      return action(active_auth(),
                    updateauth{.account = account, .permission = spending, .parent = config::active_name, .auth = auth});
   }

   /// `ram_restrictions_test::setdata`, writing a `row_bytes` row billed to the account.
   action setdata_action(uint32_t row_bytes) {
      return get_action(row_contract, "setdata"_n,
                        {{account, config::sysio_payer_name}, {account, config::active_name}},
                        mvo()("len1", row_bytes)("len2", 0)("payer", account));
   }

   /// A valid packed ABI of at least `min_size` bytes: the no-op contract's ABI with a padding clause.
   static bytes abi_of_size(size_t min_size) {
      auto abi = fc::json::from_string(test_contracts::noop_abi()).as<abi_def>();
      abi.ricardian_clauses.push_back(clause_pair{"padding", std::string(min_size, 'x')});
      return fc::raw::pack(abi);
   }

   /// A one-of-n authority over `key_count` distinct keys of the account.
   static authority keys_authority(size_t key_count) {
      authority auth;
      auth.threshold = 1;
      for (size_t i = 0; i < key_count; ++i)
         auth.keys.push_back(key_weight{get_public_key(account, "key" + std::to_string(i)), 1});
      auth.sort_fields();
      return auth;
   }

   /// RAM a permission over `key_count` K1 keys takes (K1 keys hold no bytes outside their element).
   static int64_t permission_ram(size_t key_count) {
      return static_cast<int64_t>(config::billable_size_v<permission_object> +
                                  key_count * config::billable_size_v<key_weight>);
   }

   static int64_t code_ram(const vector<uint8_t>& wasm) {
      return static_cast<int64_t>(wasm.size()) * static_cast<int64_t>(config::setcode_ram_bytes_multiplier);
   }
};

} // anonymous namespace

/*
 * register test suite `ram_tests`
 */
BOOST_AUTO_TEST_SUITE(ram_tests)

BOOST_FIXTURE_TEST_CASE(new_account_ram_tests, sysio_system::sysio_system_tester) { try {
    produce_block();

    auto& resource_manager = control->get_mutable_resource_limits_manager();
    int64_t org_sys_ram_bytes, org_sys_net_limit, org_sys_cpu_limit;
    resource_manager.get_account_limits(config::system_account_name, org_sys_ram_bytes, org_sys_net_limit, org_sys_cpu_limit);
    int64_t org_sys_ram_usage = resource_manager.get_account_ram_usage(config::system_account_name);

    create_account("kevin"_n, config::system_account_name, false, false, false, false);

    int64_t ram_bytes, net_limit, cpu_limit;
    resource_manager.get_account_limits("kevin"_n, ram_bytes, net_limit, cpu_limit);
    // RAM account limit ram_bytes is total RAM usage allowed; not what is left
    BOOST_TEST( ram_bytes == newaccount_ram ); // RAM usage allowed to account

    int64_t ram_usage = resource_manager.get_account_ram_usage("kevin"_n);
    BOOST_TEST( ram_usage == 944 ); // RAM used by "kevin" during account creation, provided by sysio

    int64_t sys_ram_bytes, sys_net_limit, sys_cpu_limit;
    resource_manager.get_account_limits(config::system_account_name, sys_ram_bytes, sys_net_limit, sys_cpu_limit);
    int64_t sys_ram_usage = resource_manager.get_account_ram_usage(config::system_account_name);
    BOOST_TEST( sys_ram_bytes == org_sys_ram_bytes - ram_bytes ); // sysio has ram_bytes less because they were provided to kevin
    BOOST_TEST( sys_ram_usage == org_sys_ram_usage ); // no change in what sysio used, the RAM was used by kevin

    produce_block(); // note onblock uses sysio to pay for blockinfo table entry

    //
    // user should be allowed to change keys
    //

    // Change active permission
    const auto new_active_priv_key = get_private_key("kevin"_n, "new_active");
    const auto new_active_pub_key = new_active_priv_key.get_public_key();
    set_authority("kevin"_n, "active"_n, authority(new_active_pub_key), "owner"_n);

    produce_blocks();

    // Change owner permission
    const auto new_owner_priv_key = get_private_key("kevin"_n, "new_owner");
    const auto new_owner_pub_key = new_owner_priv_key.get_public_key();
    set_authority("kevin"_n, "owner"_n, authority(new_owner_pub_key), {});

    produce_blocks();

    //
    // user should not have enough RAM to add additional permissions
    //
    auto spending_priv_key = get_private_key(name("kevin"), "spending");
    auto spending_pub_key = spending_priv_key.get_public_key();

    BOOST_CHECK_THROW( set_authority(name("kevin"), name("spending"), authority(spending_pub_key), name("active"),
                                     { permission_level{name("kevin"), name("active")} }, { new_active_priv_key }),
                       ram_usage_exceeded);

} FC_LOG_AND_RETHROW() }

// A freshly gifted account with fixed-size keys must be able to scope a permission to a contract,
// and linkauth is the only way to do that. If the creation gift does not cover one
// permission_link_object then such an account is born unable to link anything, so guard the
// affordance against billing drift in either the gift or the link cost.
//
// Scope: this holds for the fixed-size key types (K1, R1, EM, Ed25519), whose bytes live inside
// the authority element. A WebAuthn authority additionally carries its RPID payload on the heap
// and leaves less than a link's worth of the gift free at any RPID length -- see
// webauthn_account_rpid_bounded_by_gift below. Such accounts need a ROA policy to link, like any
// other account that outgrows the gift.
BOOST_FIXTURE_TEST_CASE(gifted_account_affords_linkauth, sysio_system::sysio_system_tester) { try {
    produce_block();

    auto& resource_manager = control->get_mutable_resource_limits_manager();
    create_account("linker"_n, config::system_account_name, false, false, false, false);

    int64_t ram_bytes, net_limit, cpu_limit;
    resource_manager.get_account_limits("linker"_n, ram_bytes, net_limit, cpu_limit);
    const int64_t usage_before = resource_manager.get_account_ram_usage("linker"_n);

    // Derived from the protocol constants rather than read back from the code under test, so a
    // billing change fails here instead of silently moving what is measured.
    constexpr int64_t expected_link_cost = 144; // 40 fixed + 3 indices x 32, aligned to 16
    // 96 account_object + 448 two permission_objects + 272 initialize_account + 64 per key_weight.
    // new_account_ram_tests pins the same 944 for K1; pinning here extends it to R1, EM and Ed25519.
    constexpr int64_t expected_fixed_key_account_ram = 96 + 448 + 272 + 2 * 64;

    constexpr int64_t link_cost = static_cast<int64_t>(config::billable_size_v<permission_link_object>);
    static_assert( link_cost == expected_link_cost,
                   "permission_link_object billing changed; a linkauth now costs a different amount of RAM" );
    BOOST_TEST( usage_before == expected_fixed_key_account_ram );
    BOOST_TEST( ram_bytes - usage_before >= link_cost ); // the gift has to leave room for one link

    // The scope claim above covers four key types, so exercise all four rather than only the K1 that
    // create_account defaults to. Each is billed the fixed element cost with no heap payload, so an
    // account built on any of them must cost the same and leave the same room.
    {
       using kt = private_key_type::key_type;
       int idx = 0;
       for( auto type : { kt::k1, kt::r1, kt::em, kt::ed } ) {
          const auto who = name{ std::string( "fixedkey" ) + std::to_string( 1111 + idx++ ) };
          const authority auth( private_key_type::generate( type ).get_public_key() );
          signed_transaction trx;
          trx.actions.emplace_back( vector<permission_level>{{config::system_account_name, config::active_name}},
                                    newaccount{ .creator = config::system_account_name, .name = who,
                                                .owner = auth, .active = auth } );
          set_transaction_headers( trx );
          trx.sign( get_private_key( config::system_account_name, "active" ), control->get_chain_id() );
          push_transaction( trx );
          produce_block();

          int64_t rb, nl, cl;
          resource_manager.get_account_limits( who, rb, nl, cl );
          BOOST_TEST( resource_manager.get_account_ram_usage( who ) == usage_before );
          BOOST_TEST( rb - resource_manager.get_account_ram_usage( who ) >= link_cost );
       }
    }

    base_tester::push_action( config::system_account_name, "linkauth"_n, "linker"_n, mvo()
                                 ("account", "linker")
                                 ("code", "sysio")
                                 ("type", "reqauth")
                                 ("requirement", "active") );
    produce_block();

    const int64_t usage_after = resource_manager.get_account_ram_usage("linker"_n);
    BOOST_TEST( usage_after - usage_before == link_cost ); // billed exactly the link object
    BOOST_TEST( usage_after <= ram_bytes );                // and still inside the gift

} FC_LOG_AND_RETHROW() }

// A WebAuthn key stores its RPID payload outside the authority element, so an authority using one
// costs more than a fixed-size key by exactly that payload. The account creation gift is therefore
// the binding constraint on RPID length: an account whose owner and active are both WebAuthn only
// fits while the extra payload stays inside newaccount_ram.
//
// The supported shape is a short RPID. Longer ones are not rejected by key validation -- post_init
// only requires a non-empty RPID -- they simply do not fit the gift and need a ROA policy like any
// other account that outgrows it. The boundary is measured and also pinned: measuring keeps the fit
// and overflow probes meaningful, pinning is what makes a billing change fail instead of silently
// sliding the boundary.
BOOST_FIXTURE_TEST_CASE(webauthn_account_rpid_bounded_by_gift, sysio_system::sysio_system_tester) { try {
    produce_block();
    auto& resource_manager = control->get_mutable_resource_limits_manager();

    fc::crypto::webauthn::public_key::public_key_data_type key_data{};
    key_data[0] = 0x02; // compressed-point prefix; contents are irrelevant to size accounting

    auto webauthn_key = [&]( size_t rpid_len ) {
       fc::crypto::webauthn::public_key k(
          key_data,
          fc::crypto::webauthn::public_key::user_presence_t::USER_PRESENCE_PRESENT,
          std::string( rpid_len, 'a' ) );
       return public_key_type( public_key_type::storage_type( std::move(k) ) );
    };

    auto create_webauthn_account = [&]( account_name who, size_t rpid_len ) {
       const authority auth( webauthn_key( rpid_len ) );
       signed_transaction trx;
       trx.actions.emplace_back( vector<permission_level>{{config::system_account_name, config::active_name}},
                                 newaccount{ .creator = config::system_account_name, .name = who,
                                             .owner = auth, .active = auth } );
       set_transaction_headers( trx );
       trx.sign( get_private_key( config::system_account_name, "active" ), control->get_chain_id() );
       push_transaction( trx );
    };

    // From the wire format, independently of the code under test: newaccount bills 816 outside its
    // keys, and each authority adds a 64-byte key_weight element plus an 8-byte shared_string header
    // over the packed key (33 point + 1 presence + 1 varint + RPID).
    constexpr int64_t webauthn_packed_len_one = 33 + 1 + 1 + 1;
    constexpr int64_t expected_at_len_one     = 816 + 2 * ( 64 + 8 + webauthn_packed_len_one ); // 1032
    constexpr int64_t expected_max_rpid_len   = 1 + ( (int64_t)newaccount_ram - expected_at_len_one ) / 2; // 57
    static_assert( expected_at_len_one == 1032 && expected_max_rpid_len == 57 );

    // Measure the shortest supported shape, then derive the longest that still fits.
    create_webauthn_account( "wakeyshort"_n, 1 );
    produce_block();
    const int64_t at_len_one = resource_manager.get_account_ram_usage( "wakeyshort"_n );
    BOOST_TEST( at_len_one == expected_at_len_one );

    const int64_t max_rpid_len = 1 + ( (int64_t)newaccount_ram - at_len_one ) / 2;
    BOOST_TEST( max_rpid_len == expected_max_rpid_len );

    // The longest supported RPID lands exactly on the gift, leaving nothing over.
    create_webauthn_account( "wakeymaxlen"_n, (size_t)max_rpid_len );
    produce_block();
    BOOST_TEST( resource_manager.get_account_ram_usage( "wakeymaxlen"_n ) == (int64_t)newaccount_ram );

    // One character more does not, and fails on RAM rather than on key validation.
    BOOST_CHECK_THROW( create_webauthn_account( "wakeytoolong"_n, (size_t)max_rpid_len + 1 ),
                       ram_usage_exceeded );

    // At every supported length a WebAuthn account has less than a link's worth of gift left, which
    // is why gifted_account_affords_linkauth is scoped to the fixed-size key types.
    int64_t ram_bytes, net_limit, cpu_limit;
    resource_manager.get_account_limits( "wakeyshort"_n, ram_bytes, net_limit, cpu_limit );
    BOOST_TEST( ram_bytes - at_len_one < (int64_t)config::billable_size_v<permission_link_object> );

} FC_LOG_AND_RETHROW() }

// Verify ram paid by contract when contract uses get_self() for payer even when explicit payer is used
BOOST_FIXTURE_TEST_CASE(auth_ram_tests, validating_tester) { try {
    using mvo = fc::mutable_variant_object;
    produce_block();

    create_accounts( {"noauthtable"_n, "alice"_n} );
    // insert uses `emplace(get_self()`, noauthtable always pays for RAM
    set_code( "noauthtable"_n, test_contracts::no_auth_table_wasm() );
    set_abi( "noauthtable"_n, test_contracts::no_auth_table_abi() );
    produce_block();

    const resource_limits_manager& mgr = control->get_resource_limits_manager();
    auto noauthtable_cpu_limit0 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit0 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit0 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit0 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage0 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage0 = mgr.get_account_ram_usage("alice"_n);

    vector<permission_level> noauthtable_auth {{"noauthtable"_n, config::active_name} };
    // noauthtable pays for CPU,NET
    push_action( "noauthtable"_n, "insert"_n, noauthtable_auth, mvo()("user", "a") ("id", 1) ("age", 10));

    auto noauthtable_cpu_limit1 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit1 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit1 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit1 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage1 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage1 = mgr.get_account_ram_usage("alice"_n);

    BOOST_TEST(noauthtable_cpu_limit0 < noauthtable_cpu_limit1);
    BOOST_TEST(alice_cpu_limit0 == alice_cpu_limit1);
    BOOST_TEST(noauthtable_net_limit0 < noauthtable_net_limit1);
    BOOST_TEST(alice_net_limit0 == alice_net_limit1);
    BOOST_TEST(noauthtable_ram_usage0 < noauthtable_ram_usage1);
    BOOST_TEST(alice_ram_usage0 == alice_ram_usage1);

    vector<permission_level> alice_auth {{"alice"_n, config::active_name} };
    // noauthtable still pays for CPU,NET
    push_action( "noauthtable"_n, "insert"_n, alice_auth, mvo()("user", "b") ("id", 1) ("age", 10));

    auto noauthtable_cpu_limit2 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit2 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit2 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit2 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage2 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage2 = mgr.get_account_ram_usage("alice"_n);

    BOOST_TEST(noauthtable_cpu_limit1 < noauthtable_cpu_limit2);
    BOOST_TEST(alice_cpu_limit1 == alice_cpu_limit2);
    BOOST_TEST(noauthtable_net_limit1 < noauthtable_net_limit2);
    BOOST_TEST(alice_net_limit1 == alice_net_limit2);
    BOOST_TEST(noauthtable_ram_usage1 < noauthtable_ram_usage2);
    BOOST_TEST(alice_ram_usage1 == alice_ram_usage2);

    vector<permission_level> alice_explicit {{"alice"_n, config::sysio_payer_name}, {"alice"_n, config::active_name} };
    // alice pays for CPU,NET
    push_action( "noauthtable"_n, "insert"_n, alice_explicit, mvo()("user", "c") ("id", 1) ("age", 10));

    auto noauthtable_cpu_limit3 = mgr.get_account_cpu_limit_ex("noauthtable"_n).first.current_used;
    auto alice_cpu_limit3 = mgr.get_account_cpu_limit_ex("alice"_n).first.current_used;
    auto noauthtable_net_limit3 = mgr.get_account_net_limit_ex("noauthtable"_n).first.current_used;
    auto alice_net_limit3 = mgr.get_account_net_limit_ex("alice"_n).first.current_used;
    auto noauthtable_ram_usage3 = mgr.get_account_ram_usage("noauthtable"_n);
    auto alice_ram_usage3 = mgr.get_account_ram_usage("alice"_n);

    BOOST_TEST(noauthtable_cpu_limit2 == noauthtable_cpu_limit3);
    BOOST_TEST(alice_cpu_limit2 < alice_cpu_limit3);
    BOOST_TEST(noauthtable_net_limit2 == noauthtable_net_limit3);
    BOOST_TEST(alice_net_limit2 < alice_net_limit3);
    BOOST_TEST(noauthtable_ram_usage2 < noauthtable_ram_usage3);
    BOOST_TEST(alice_ram_usage2 == alice_ram_usage3);
} FC_LOG_AND_RETHROW() }

// Permission lifecycle RAM billing -- exact-delta tests that verify the
// sysio_contract.cpp billing flows use billable_size_v<permission_object>,
// billable_size_v<permission_link_object>, and shared_authority::get_billable_size()
// correctly.

BOOST_FIXTURE_TEST_CASE(linkauth_unlinkauth_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    // Alice needs a non-active permission to link.
    const auto spending_pub_key = get_public_key("alice"_n, "spending");
    set_authority("alice"_n, "spending"_n, authority(spending_pub_key), "active"_n);
    produce_block();

    const auto& rlm = control->get_resource_limits_manager();
    auto before_link = rlm.get_account_ram_usage("alice"_n);

    link_authority("alice"_n, "sysio"_n, "spending"_n, "reqauth"_n);
    produce_block();

    auto after_link = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_link - before_link,
                        static_cast<int64_t>(config::billable_size_v<permission_link_object>));

    unlink_authority("alice"_n, "sysio"_n, "reqauth"_n);
    produce_block();

    auto after_unlink = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_unlink - after_link,
                        -static_cast<int64_t>(config::billable_size_v<permission_link_object>));
    BOOST_REQUIRE_EQUAL(after_unlink, before_link); // full roundtrip
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(createauth_deleteauth_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    const auto& rlm = control->get_resource_limits_manager();
    auto before_create = rlm.get_account_ram_usage("alice"_n);

    const auto spending_pub_key = get_public_key("alice"_n, "spending");
    set_authority("alice"_n, "spending"_n, authority(spending_pub_key), "active"_n);
    produce_block();

    // Fetch the freshly created permission to compute its exact billable size.
    const auto* perm = control->db().find<permission_object, by_owner>(
        boost::make_tuple("alice"_n, "spending"_n));
    BOOST_REQUIRE(perm != nullptr);

    const int64_t expected_create_bill =
        static_cast<int64_t>(config::billable_size_v<permission_object> + perm->auth.get_billable_size());

    auto after_create = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_create - before_create, expected_create_bill);

    delete_authority("alice"_n, "spending"_n);
    produce_block();

    auto after_delete = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_delete - after_create, -expected_create_bill);
    BOOST_REQUIRE_EQUAL(after_delete, before_create); // full roundtrip
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(updateauth_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    // Start with a single-key spending permission.
    const auto key1 = get_public_key("alice"_n, "spending1");
    set_authority("alice"_n, "spending"_n, authority(key1), "active"_n);
    produce_block();

    const auto* perm_before = control->db().find<permission_object, by_owner>(
        boost::make_tuple("alice"_n, "spending"_n));
    BOOST_REQUIRE(perm_before != nullptr);
    const int64_t old_auth_bill = perm_before->auth.get_billable_size();

    const auto& rlm = control->get_resource_limits_manager();
    auto before_update = rlm.get_account_ram_usage("alice"_n);

    // Grow the authority to two keys. permission_object fixed size is
    // unchanged; only the auth bill should grow.
    const auto key2 = get_public_key("alice"_n, "spending2");
    authority two_key_auth(1,
                           { key_weight{key1, 1}, key_weight{key2, 1} },
                           {});
    two_key_auth.sort_fields(); // validate() requires keys in strict order
    set_authority("alice"_n, "spending"_n, two_key_auth, "active"_n);
    produce_block();

    const auto* perm_after = control->db().find<permission_object, by_owner>(
        boost::make_tuple("alice"_n, "spending"_n));
    BOOST_REQUIRE(perm_after != nullptr);
    const int64_t new_auth_bill = perm_after->auth.get_billable_size();

    auto after_update = rlm.get_account_ram_usage("alice"_n);
    BOOST_REQUIRE_EQUAL(after_update - before_update, new_auth_bill - old_auth_bill);
    BOOST_REQUIRE_GT(new_auth_bill, old_auth_bill); // sanity: growing added bytes
} FC_LOG_AND_RETHROW() }

// setcode / setabi RAM billing -- exact-delta tests covering
// billable_size_v<account_metadata_object> and setcode_ram_bytes_multiplier.

BOOST_FIXTURE_TEST_CASE(setcode_first_time_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    // Confirm precondition: no account_metadata_object yet.
    const auto* md_before = control->db().find<account_metadata_object, by_name>("alice"_n);
    BOOST_REQUIRE(md_before == nullptr);

    const auto& rlm = control->get_resource_limits_manager();
    auto before = rlm.get_account_ram_usage("alice"_n);

    const auto& wasm = test_contracts::no_auth_table_wasm();
    set_code("alice"_n, wasm);
    produce_block();

    auto after = rlm.get_account_ram_usage("alice"_n);

    // sysio.bios records the deploy in its codehash table, billed to the account: an owner key and a row of
    // owner, sha256, block time and block number (sysio.system/account_changes.hpp).
    constexpr int64_t codehash_key_bytes = sizeof(uint64_t);
    constexpr int64_t codehash_row_bytes = sizeof(uint64_t) + sizeof(fc::sha256) + sizeof(int64_t) + sizeof(uint32_t);
    constexpr int64_t kv_row_overhead = 112; // a protocol price, so a literal rather than the config constant
    const int64_t codehash_billable = codehash_key_bytes + codehash_row_bytes + kv_row_overhead;

    // First setcode on an account: bill = wasm.size() * multiplier + metadata + the codehash row.
    int64_t expected =
          static_cast<int64_t>(wasm.size()) * static_cast<int64_t>(config::setcode_ram_bytes_multiplier)
        + static_cast<int64_t>(config::billable_size_v<account_metadata_object>)
        + codehash_billable;
    BOOST_REQUIRE_EQUAL(after - before, expected);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setcode_update_ram_billing, validating_tester) { try {
    create_account("alice"_n);
    produce_block();

    // First deploy establishes the metadata object and the initial code bill.
    const auto& old_wasm = test_contracts::no_auth_table_wasm();
    set_code("alice"_n, old_wasm);
    produce_block();
    const int64_t old_code_size = static_cast<int64_t>(old_wasm.size());

    const auto& rlm = control->get_resource_limits_manager();
    auto before_update = rlm.get_account_ram_usage("alice"_n);

    // Second deploy swaps to a different contract -- metadata already exists,
    // so only the (new_code - old_code) * multiplier delta is billed.
    const auto& new_wasm = test_contracts::noop_wasm();
    set_code("alice"_n, new_wasm);
    produce_block();

    auto after_update = rlm.get_account_ram_usage("alice"_n);

    int64_t expected =
        (static_cast<int64_t>(new_wasm.size()) - old_code_size)
      * static_cast<int64_t>(config::setcode_ram_bytes_multiplier);
    BOOST_REQUIRE_EQUAL(after_update - before_update, expected);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(setabi_first_time_ram_billing) { try {
    // Use preactivate_feature_only so the bios contract is NOT deployed at
    // sysio -- the bios setabi handler inserts an abi_hash_table row which
    // would double-bill alice through its own kv_object ram usage.
    validating_tester t(flat_set<account_name>{}, nullptr, setup_policy::preactivate_feature_only);
    t.create_account("alice"_n);
    t.produce_block();

    // Confirm precondition: no account_metadata_object yet.
    const auto* md_before = t.control->db().find<account_metadata_object, by_name>("alice"_n);
    BOOST_REQUIRE(md_before == nullptr);

    const auto& rlm = t.control->get_resource_limits_manager();
    auto before = rlm.get_account_ram_usage("alice"_n);

    t.set_abi("alice"_n, test_contracts::no_auth_table_abi());
    t.produce_block();

    const auto* md = t.control->db().find<account_metadata_object, by_name>("alice"_n);
    BOOST_REQUIRE(md != nullptr);
    const int64_t stored_abi_size = static_cast<int64_t>(md->abi.size());

    auto after = rlm.get_account_ram_usage("alice"_n);
    int64_t expected = stored_abi_size + static_cast<int64_t>(config::billable_size_v<account_metadata_object>);
    BOOST_REQUIRE_EQUAL(after - before, expected);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(setabi_update_ram_billing) { try {
    // See setabi_first_time_ram_billing for why setup_policy::full would
    // double-bill through the bios setabi notification handler.
    validating_tester t(flat_set<account_name>{}, nullptr, setup_policy::preactivate_feature_only);
    t.create_account("alice"_n);
    t.produce_block();

    t.set_abi("alice"_n, test_contracts::no_auth_table_abi());
    t.produce_block();
    const auto* md1 = t.control->db().find<account_metadata_object, by_name>("alice"_n);
    BOOST_REQUIRE(md1 != nullptr);
    const int64_t old_abi_size = static_cast<int64_t>(md1->abi.size());

    const auto& rlm = t.control->get_resource_limits_manager();
    auto before_update = rlm.get_account_ram_usage("alice"_n);

    t.set_abi("alice"_n, test_contracts::noop_abi());
    t.produce_block();
    const auto* md2 = t.control->db().find<account_metadata_object, by_name>("alice"_n);
    BOOST_REQUIRE(md2 != nullptr);
    const int64_t new_abi_size = static_cast<int64_t>(md2->abi.size());

    auto after_update = rlm.get_account_ram_usage("alice"_n);

    // Metadata already exists from the first setabi, so the update only bills
    // the raw serialized-size delta.
    BOOST_REQUIRE_EQUAL(after_update - before_update, new_abi_size - old_abi_size);
} FC_LOG_AND_RETHROW() }

// Native setcode, setabi and updateauth check the account's RAM quota when they grow it, not only at the end of the
// transaction.

BOOST_FIXTURE_TEST_CASE(setcode_checks_ram_quota_when_it_runs, native_ram_quota_fixture) { try {
    const auto& wasm = test_contracts::no_auth_table_wasm();
    set_ram_headroom(code_ram(wasm) - 1);
    const auto usage_before = ram_usage();

    // The code does not fit, so the transaction fails even though its second action clears the code again.
    BOOST_REQUIRE_EXCEPTION(push_actions({setcode_action(wasm), setcode_action({})}), ram_usage_exceeded,
                            fc_exception_message_contains("insufficient ram"));
    BOOST_REQUIRE_EQUAL(ram_usage(), usage_before);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setabi_checks_ram_quota_when_it_runs, native_ram_quota_fixture) { try {
    constexpr int64_t headroom = 1024;
    set_ram_headroom(headroom);
    const auto abi = abi_of_size(2 * headroom);
    const auto usage_before = ram_usage();

    BOOST_REQUIRE_EXCEPTION(push_actions({setabi_action(abi), setabi_action({})}), ram_usage_exceeded,
                            fc_exception_message_contains("insufficient ram"));
    BOOST_REQUIRE_EQUAL(ram_usage(), usage_before);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(updateauth_checks_ram_quota_when_it_runs, native_ram_quota_fixture) { try {
    constexpr size_t key_count = 64;
    const auto large = keys_authority(key_count);
    const auto small = keys_authority(1);

    // A row billed to the account, which a later action can free.
    const auto row_bytes = static_cast<uint32_t>(2 * permission_ram(key_count));
    set_ram_headroom(2 * static_cast<int64_t>(row_bytes));
    push_actions({setdata_action(row_bytes)});
    produce_block();
    set_ram_headroom(permission_ram(key_count) / 2);
    const auto usage_before = ram_usage();

    // Creating a permission that does not fit fails, even when the next action frees more than it takes.
    BOOST_REQUIRE_EXCEPTION(push_actions({updateauth_action(large), setdata_action(0)}), ram_usage_exceeded,
                            fc_exception_message_contains("insufficient ram"));
    BOOST_REQUIRE_EQUAL(ram_usage(), usage_before);

    // Growing an existing permission past the quota fails, even when the next action shrinks it back.
    push_actions({updateauth_action(small)});
    produce_block();
    const auto usage_with_small = ram_usage();
    BOOST_REQUIRE_EXCEPTION(push_actions({updateauth_action(large), updateauth_action(small)}), ram_usage_exceeded,
                            fc_exception_message_contains("insufficient ram"));
    BOOST_REQUIRE_EQUAL(ram_usage(), usage_with_small);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(native_actions_within_ram_quota_succeed, native_ram_quota_fixture) { try {
    const auto& large_wasm = test_contracts::no_auth_table_wasm();
    const auto& small_wasm = test_contracts::noop_wasm();
    BOOST_REQUIRE_GT(large_wasm.size(), small_wasm.size());
    const auto abi = abi_of_size(1024);
    // Room for the larger contract, the ABI, the hash rows the system contract records, and a permission.
    constexpr int64_t slack = 4096;
    set_ram_headroom(code_ram(large_wasm) + static_cast<int64_t>(abi.size()) + slack);

    // setcode and setabi in one transaction.
    push_actions({setcode_action(small_wasm), setabi_action(abi)});
    produce_block();

    // Replacing the code with larger code, then with smaller code.
    push_actions({setcode_action(large_wasm)});
    produce_block();
    push_actions({setcode_action(small_wasm)});
    produce_block();

    // An authority that fits.
    push_actions({updateauth_action(keys_authority(4))});
    produce_block();

    const auto* metadata = control->find_account_metadata(account);
    BOOST_REQUIRE(metadata != nullptr);
    BOOST_REQUIRE(metadata->code_hash == fc::sha256::hash(reinterpret_cast<const char*>(small_wasm.data()),
                                                          static_cast<uint32_t>(small_wasm.size())));
    BOOST_REQUIRE_EQUAL(metadata->abi.size(), abi.size());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(native_actions_count_ram_freed_earlier_in_the_transaction, native_ram_quota_fixture) { try {
    const auto& wasm = test_contracts::no_auth_table_wasm();
    constexpr int64_t slack = 1024;
    set_ram_headroom(code_ram(wasm) + slack);
    push_actions({setcode_action(wasm)});
    produce_block();
    set_ram_headroom(0);

    // Clearing the code first frees enough RAM for an ABI half its size in the same transaction.
    const auto abi = abi_of_size(static_cast<size_t>(code_ram(wasm) / 2));
    push_actions({setcode_action({}), setabi_action(abi)});
    produce_block();

    const auto* metadata = control->find_account_metadata(account);
    BOOST_REQUIRE(metadata != nullptr);
    BOOST_REQUIRE(metadata->code_hash == digest_type());
    BOOST_REQUIRE_EQUAL(metadata->abi.size(), abi.size());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(native_shrink_passes_while_account_is_over_ram_quota, native_ram_quota_fixture) { try {
    const auto& wasm = test_contracts::no_auth_table_wasm();
    const auto abi = abi_of_size(1024);
    constexpr int64_t slack = 4096;
    set_ram_headroom(code_ram(wasm) + static_cast<int64_t>(abi.size()) + slack);
    push_actions({setcode_action(wasm), setabi_action(abi), updateauth_action(keys_authority(8))});
    produce_block();
    set_ram_headroom(0);

    // A row billed to the account puts it over its quota for the rest of the transaction. Clearing code, clearing
    // the ABI and shrinking an authority all release RAM, so they pass; the last action frees the row again.
    constexpr uint32_t row_bytes = 2048;
    push_actions({setdata_action(row_bytes), setcode_action({}), setabi_action({}),
                  updateauth_action(keys_authority(1)), setdata_action(0)});
    produce_block();

    // Growing RAM while over the quota fails, even though clearing the code later in the transaction would leave the
    // account within it.
    set_ram_headroom(code_ram(wasm) + slack);
    push_actions({setcode_action(wasm)});
    produce_block();
    set_ram_headroom(0);
    BOOST_REQUIRE_EXCEPTION(push_actions({setdata_action(row_bytes), setabi_action(abi_of_size(64)),
                                          setcode_action({}), setdata_action(0)}),
                            ram_usage_exceeded, fc_exception_message_contains("insufficient ram"));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
