#include <boost/test/unit_test.hpp>

#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include "contracts.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

using mvo = fc::mutable_variant_object;

/// End-to-end coverage for a contract driving the *native* auth actions through `sysio.code`.
///
/// The account holder grants `<contract>@sysio.code` a seat in a code-only PARENT permission and
/// puts the session key in a key-only child beneath it. The contract can then inline-send
/// `unlinkauth` / `deleteauth` / `updateauth` naming `{holder, parent}` as the authorization,
/// because permission_object::satisfies walks up the target's ancestry, so the parent satisfies
/// every descendant.
///
/// IMPORTANT -- reaping is NOT guaranteed. It is opportunistic cleanup that works against a
/// cooperative key holder, not a boundary enforceable against a hostile one. In one transaction a
/// session key can unlink itself, delete itself, recreate the same permission name as a SIBLING of
/// the gate, and relink -- every action authorized against pre-state, where the old child still
/// exists. The recreated permission is outside the gate's subtree and the contract can never clean
/// it up (see `session_key_escapes_gate_by_recreation`).
///
/// What that attack does NOT buy is authority: the escaped permission still only satisfies what it
/// is linked to, so a real TTL has to be enforced by the consuming contract at use time. Treat the
/// parent split as hygiene and RAM recovery as best-effort.
///
/// The unit-level counterparts in `unittests/auth_tests.cpp` pin the authorization decision on its
/// own; these cases drive the whole path -- apply_context::execute_inline, native handler dispatch,
/// resulting state change and RAM refund.
namespace {

/// Session-key topology: a code-only parent over a key-only child.
///
///   alice@sessgate (parent active)   -- accounts [{sessmgr, sysio.code}], NO keys
///   alice@session  (parent sessgate) -- keys [session key], NO code entry
///
/// The seat lives in the parent so the session key cannot evict it in place -- updateauth requires
/// satisfying the permission being changed, and a child never satisfies its parent. The reaper
/// satisfies every descendant of sessgate, since satisfies() walks up the target's ancestry.
///
/// This stops in-place eviction, not escape: see session_key_escapes_gate_by_recreation.
constexpr auto code_parent  = "sessgate"_n;
constexpr auto key_child    = "session"_n;
constexpr auto alice        = "alice"_n;
constexpr auto sessmgr      = "sessmgr"_n;
constexpr auto mallory      = "mallory"_n;
/// Third party with no relationship to alice; signs the outer transaction to show the reap is
/// permissionless.
constexpr auto reaper       = "reaper"_n;

/// Deploy the generic inline-action forwarder onto @p account.
void deploy_sendinline( validating_tester& chain, account_name account ) {
   chain.set_code( account, system_contracts::testing::test_contracts::sendinline_wasm() );
   chain.set_abi( account, system_contracts::testing::test_contracts::sendinline_abi().data() );
}

/// Push `sendinline::send` from @p contract, forwarding @p payload to `sysio::<action_name>` under
/// the single authorization @p auth.
transaction_trace_ptr forward_native( validating_tester& chain,
                                      account_name contract,
                                      action_name action_name_,
                                      permission_level auth,
                                      const bytes& payload ) {
   return chain.push_action( contract, "send"_n, reaper, mvo()
                                ( "contract", config::system_account_name )
                                ( "action_name", action_name_ )
                                ( "auths", std::vector<permission_level>{ auth } )
                                ( "payload", payload ) );
}

struct inline_auth_tester : validating_tester {
   inline_auth_tester() {
      create_accounts( {alice, sessmgr, mallory, reaper} );
      produce_block();
      deploy_sendinline( *this, sessmgr );
      deploy_sendinline( *this, mallory );
      produce_block();

      const authority gate_auth( 1, {}, {{{sessmgr, config::sysio_code_name}, 1}} );
      set_authority( alice, code_parent, gate_auth, config::active_name );
      const authority session_auth( 1, {{get_public_key( alice, "session" ), 1}}, {} );
      set_authority( alice, key_child, session_auth, code_parent );
      produce_block();
   }

   const permission_object* find_perm( permission_name p ) const {
      return control->get_authorization_manager().find_permission( {alice, p} );
   }

   int64_t alice_ram() const {
      return control->get_resource_limits_manager().get_account_ram_usage( alice );
    }

   /// Reap @p perm as the contract would: declare the code-only parent, which satisfies it.
   transaction_trace_ptr reap( account_name contract, permission_name perm ) {
      return forward_native( *this, contract, deleteauth::get_name(), {alice, code_parent},
                             fc::raw::pack( deleteauth{ alice, perm } ) );
   }
};

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(sysio_inline_auth_tests)

BOOST_FIXTURE_TEST_CASE( reap_unlinks_and_deletes_session, inline_auth_tester ) try {
   link_authority( alice, config::system_account_name, key_child, "reqauth"_n );
   produce_block();
   BOOST_REQUIRE( find_perm( key_child ) != nullptr );

   // Neither transaction carries a signature from alice: reaper signs the outer trx and the
   // sysio.code seat in the parent authorizes both inline actions.
   forward_native( *this, sessmgr, unlinkauth::get_name(), {alice, code_parent},
                   fc::raw::pack( unlinkauth{ alice, config::system_account_name, "reqauth"_n } ) );
   produce_block();
   reap( sessmgr, key_child );
   produce_block();

   BOOST_REQUIRE( find_perm( key_child ) == nullptr );
   BOOST_REQUIRE( find_perm( code_parent ) != nullptr ); // gate survives its child
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( session_key_cannot_eject_reaper, inline_auth_tester ) try {
   // The session key may rewrite its own authority -- a permission satisfies itself. Under the
   // unsafe single-permission topology that would drop the contract's seat and strand cleanup.
   // Here the seat is in the parent, so rotating the child changes nothing about who can reap.
   const authority rotated( 1, {{get_public_key( alice, "rotated" ), 1}}, {} );
   set_authority( alice, key_child, rotated, code_parent,
                  { permission_level{alice, key_child} }, { get_private_key( alice, "session" ) } );
   produce_block();

   // The child also cannot reach up and rewrite or delete the gate.
   BOOST_REQUIRE_EXCEPTION(
      set_authority( alice, code_parent, authority( get_public_key( alice, "attacker" ) ), config::active_name,
                     { permission_level{alice, key_child} }, { get_private_key( alice, "rotated" ) } ),
      irrelevant_auth_exception,
      fc_exception_message_starts_with( "updateauth action declares irrelevant authority" ) );

   // Reaper still wins.
   reap( sessmgr, key_child );
   produce_block();
   BOOST_REQUIRE( find_perm( key_child ) == nullptr );
} FC_LOG_AND_RETHROW()

// Characterizes the topology this suite exists to avoid: session key and sysio.code seat in ONE
// permission. Because a permission satisfies itself, the key can rewrite that authority, drop the
// seat, and lock the contract out -- while any linkauth on it survives untouched. This is why the
// seat belongs in a parent the key cannot reach.
BOOST_FIXTURE_TEST_CASE( combined_permission_lets_key_evict_reaper, inline_auth_tester ) try {
   constexpr auto keeps_seat = "combineda"_n;
   constexpr auto drops_seat = "combinedb"_n;
   const auto seat = permission_level_weight{ {sessmgr, config::sysio_code_name}, 1 };

   for( auto perm : { keeps_seat, drops_seat } ) {
      set_authority( alice, perm, authority( 1, {{get_public_key( alice, perm.to_string() ), 1}}, {seat} ),
                     config::active_name );
   }
   produce_block();

   // Untouched, the contract reaps it: the seat is present and the permission satisfies itself.
   forward_native( *this, sessmgr, deleteauth::get_name(), {alice, keeps_seat},
                   fc::raw::pack( deleteauth{ alice, keeps_seat } ) );
   produce_block();
   BOOST_REQUIRE( find_perm( keeps_seat ) == nullptr );

   // The key rewrites its own authority and drops the seat -- authorized, because the permission
   // being updated is the one doing the authorizing.
   set_authority( alice, drops_seat, authority( get_public_key( alice, "evicted" ) ), config::active_name,
                  { permission_level{alice, drops_seat} }, { get_private_key( alice, drops_seat.to_string() ) } );
   produce_block();

   // Same contract, same action, now locked out.
   BOOST_REQUIRE_EXCEPTION(
      forward_native( *this, sessmgr, deleteauth::get_name(), {alice, drops_seat},
                      fc::raw::pack( deleteauth{ alice, drops_seat } ) ),
      unsatisfied_authorization,
      fc_exception_message_starts_with( "transaction declares authority" ) );
   BOOST_REQUIRE( find_perm( drops_seat ) != nullptr );
} FC_LOG_AND_RETHROW()

// Transaction authorization is decided for every top-level action up front, against pre-state,
// before any of them executes. A session key can exploit that to leave the gate's subtree entirely:
// unlink, delete, recreate under a different parent, relink -- all in one transaction it signs
// alone. Precheck sees the original child for every action, so every action is authorized; by
// execution time the delete has already run, so updateauth takes its create branch and accepts the
// new parent. The "Changing parent authority is not currently supported" guard never fires because
// it lives inside `if (permission)`.
//
// The consequence is that reaping cannot be presented as an enforceable TTL boundary. It is
// cleanup for a cooperative holder. Note also what the attack does NOT achieve: the recreated
// permission is still only usable for what it is linked to, so authority is unchanged -- what is
// lost is the contract's ability to ever clean it up.
BOOST_FIXTURE_TEST_CASE( session_key_escapes_gate_by_recreation, inline_auth_tester ) try {
   link_authority( alice, config::system_account_name, key_child, "reqauth"_n );
   produce_block();

   const auto* gate_before = find_perm( code_parent );
   BOOST_REQUIRE( gate_before != nullptr );
   BOOST_REQUIRE( find_perm( key_child )->parent == gate_before->id ); // child of the gate

   const authority same_key( 1, {{get_public_key( alice, "session" ), 1}}, {} );
   const vector<permission_level> as_child{{alice, key_child}};

   signed_transaction trx;
   trx.actions.emplace_back( as_child, unlinkauth{ alice, config::system_account_name, "reqauth"_n } );
   trx.actions.emplace_back( as_child, deleteauth{ alice, key_child } );
   trx.actions.emplace_back( as_child, updateauth{ .account    = alice,
                                                   .permission = key_child,
                                                   .parent     = config::active_name,  // out of the subtree
                                                   .auth       = same_key } );
   trx.actions.emplace_back( as_child, linkauth{ alice, config::system_account_name, "reqauth"_n, key_child } );
   set_transaction_headers( trx );
   trx.sign( get_private_key( alice, "session" ), control->get_chain_id() );
   push_transaction( trx );
   produce_block();

   // Same name, same key, now a sibling of the gate rather than its child.
   const auto* escaped = find_perm( key_child );
   BOOST_REQUIRE( escaped != nullptr );
   BOOST_REQUIRE( escaped->parent != find_perm( code_parent )->id );

   // The gate no longer satisfies it, so the contract is locked out for good.
   // Matched loosely on purpose: the failing action here is deleteauth, and the assert text in
   // check_deleteauth_authorization is being corrected separately.
   BOOST_REQUIRE_EXCEPTION( reap( sessmgr, key_child ),
                            irrelevant_auth_exception,
                            fc_exception_message_contains( "declares irrelevant authority" ) );
   BOOST_REQUIRE( find_perm( key_child ) != nullptr );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( code_parent_can_update_child, inline_auth_tester ) try {
   // Positive updateauth through sysio.code: the gate rotates its child's key.
   const auto rotated_pub = get_public_key( alice, "rotated" );
   forward_native( *this, sessmgr, updateauth::get_name(), {alice, code_parent},
                   fc::raw::pack( updateauth{ .account    = alice,
                                              .permission = key_child,
                                              .parent     = code_parent,
                                              .auth       = authority( rotated_pub ) } ) );
   produce_block();

   const auto* child = find_perm( key_child );
   BOOST_REQUIRE( child != nullptr );
   BOOST_REQUIRE_EQUAL( child->auth.keys.size(), 1u );
   BOOST_REQUIRE( child->auth.keys[0].key == rotated_pub );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( descendant_defers_but_does_not_block_reap, inline_auth_tester ) try {
   // The session key can create a child under itself, and a permission with children cannot be
   // removed -- so RAM recovery is best-effort in timing, not guaranteed immediate. It is not a
   // permanent block: the gate satisfies grandchildren too, so the reaper unwinds depth-first.
   constexpr auto grandchild = "sessgrand"_n;
   set_authority( alice, grandchild, authority( get_public_key( alice, "grand" ) ), key_child,
                  { permission_level{alice, key_child} }, { get_private_key( alice, "session" ) } );
   produce_block();

   BOOST_REQUIRE_EXCEPTION( reap( sessmgr, key_child ),
                            action_validate_exception,
                            fc_exception_message_starts_with( "Cannot remove a permission which has children" ) );

   reap( sessmgr, grandchild );   // gate satisfies the grandchild
   produce_block();
   reap( sessmgr, key_child );
   produce_block();

   BOOST_REQUIRE( find_perm( grandchild ) == nullptr );
   BOOST_REQUIRE( find_perm( key_child ) == nullptr );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( reap_refunds_ram_to_holder, inline_auth_tester ) try {
   const int64_t baseline = alice_ram();

   constexpr auto second_perm = "session2"_n;
   set_authority( alice, second_perm, authority( get_public_key( alice, "session2" ) ), code_parent );
   produce_block();
   BOOST_REQUIRE_GT( alice_ram(), baseline );

   reap( sessmgr, second_perm );
   produce_block();

   BOOST_REQUIRE( find_perm( second_perm ) == nullptr );
   // The permission's RAM goes back to alice in full, not to the contract that removed it.
   BOOST_REQUIRE_EQUAL( alice_ram(), baseline );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( deleteauth_blocked_while_linked, inline_auth_tester ) try {
   link_authority( alice, config::system_account_name, key_child, "reqauth"_n );
   produce_block();

   BOOST_REQUIRE_EXCEPTION( reap( sessmgr, key_child ),
                            action_validate_exception,
                            fc_exception_message_starts_with( "Cannot delete a linked authority" ) );

   BOOST_REQUIRE( find_perm( key_child ) != nullptr );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( foreign_contract_cannot_reap, inline_auth_tester ) try {
   // mallory runs the identical forwarder but holds no seat in alice's gate.
   BOOST_REQUIRE_EXCEPTION( reap( mallory, key_child ),
                            unsatisfied_authorization,
                            fc_exception_message_starts_with( "transaction declares authority" ) );

   BOOST_REQUIRE( find_perm( key_child ) != nullptr );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( code_grant_cannot_rewrite_active, inline_auth_tester ) try {
   // The seat sits on a child of active. If it could reach active, a scoped grant would be a full
   // account takeover.
   BOOST_REQUIRE_EXCEPTION(
      forward_native( *this, sessmgr, updateauth::get_name(), {alice, code_parent},
                      fc::raw::pack( updateauth{ .account    = alice,
                                                 .permission = config::active_name,
                                                 .parent     = config::owner_name,
                                                 .auth       = authority( get_public_key( alice, "attacker" ) ) } ) ),
      irrelevant_auth_exception,
      fc_exception_message_starts_with( "updateauth action declares irrelevant authority" ) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
