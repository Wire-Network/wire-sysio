#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"

#include <string>
#include <string_view>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;

using mvo = fc::mutable_variant_object;
namespace andon_support = sysio_system::test_support::andon;

namespace {

/// Refusals of sysio.andon.
constexpr std::string_view NOT_ACCOUNT      = "account does not exist";
constexpr std::string_view ALREADY_PULLER   = "contract is already a puller";
constexpr std::string_view TOO_MANY_PULLERS = "too many pullers";
constexpr std::string_view PULL_AUTHORITY   = "only the panic account, sysio or a registered puller may pull";
constexpr std::string_view CLEAR_AUTHORITY  = "only the panic account or sysio may clear";
/// What a pull of a pulled cord and a clear of a clear one print.
constexpr std::string_view ALREADY_PULLED = "sysio.andon::pull: the cord is already pulled; nothing changes";
constexpr std::string_view ALREADY_CLEAR  = "sysio.andon::clear: the cord is not pulled; nothing changes";
/// sysio.andon's MAX_PULLERS and MAX_TEXT_BYTES.
constexpr uint32_t MAX_PULLERS    = 16;
constexpr size_t   MAX_TEXT_BYTES = 256;

} // anonymous namespace

/// sysio.andon alone, with a stand-in on sysio.epoch that moves the depot epoch index the cord records.
/// The contracts the cord freezes are exercised in their own suites.
class sysio_andon_tester : public tester {
public:
   static constexpr auto ANDON_ACCOUNT = "sysio.andon"_n;
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto SYSIO_ACCOUNT = "sysio"_n;
   static constexpr auto PANIC         = "panic"_n;
   static constexpr auto PULLER        = "puller"_n;
   static constexpr auto ALICE         = "alice"_n;

   sysio_andon_tester() {
      produce_blocks(2);
      create_accounts({ANDON_ACCOUNT, EPOCH_ACCOUNT, PANIC, PULLER, ALICE});
      produce_blocks(2);
      andon_support::deploy(*this, andon_abi_ser, contracts::andon_wasm(), contracts::andon_abi());
      set_code(EPOCH_ACCOUNT, contracts::util::epoch_stub_wasm());
      set_abi(EPOCH_ACCOUNT, contracts::util::epoch_stub_abi().data());
      produce_blocks();
      sysio_system::test_support::load_account_abi(*this, EPOCH_ACCOUNT, epoch_abi_ser);
   }

   /// Push one action in its own block, keeping its trace in `last_trace` (null after a failure).
   action_result push(name code, abi_serializer& ser, name signer, name action_name, const variant_object& data) {
      return sysio_system::test_support::push_contract_action_and_produce_block(*this, code, ser, signer,
                                                                                action_name, data, last_trace);
   }
   /// Every action's console of the last pushed transaction.
   std::string console() const {
      std::string out;
      if (!last_trace) return out;
      for (const auto& at : last_trace->action_traces) out += at.console;
      return out;
   }

   action_result setpanic(name account, name signer = SYSIO_ACCOUNT) {
      return push(ANDON_ACCOUNT, andon_abi_ser, signer, "setpanic"_n, mvo()("account", account));
   }
   action_result addpuller(name contract, name signer = SYSIO_ACCOUNT) {
      return push(ANDON_ACCOUNT, andon_abi_ser, signer, "addpuller"_n, mvo()("contract", contract));
   }
   /// Pull as `actor`, signed by `signer` (the actor by default).
   action_result pull(name actor, const std::string& reason = "incident", name signer = name{}) {
      return push(ANDON_ACCOUNT, andon_abi_ser, signer == name{} ? actor : signer, "pull"_n,
                  mvo()("actor", actor)("reason", reason));
   }
   /// Clear as `actor`, signed by `signer` (the actor by default).
   action_result clear(name actor, const std::string& note = "resolved", name signer = name{}) {
      return push(ANDON_ACCOUNT, andon_abi_ser, signer == name{} ? actor : signer, "clear"_n,
                  mvo()("actor", actor)("note", note));
   }
   /// Move the depot's current epoch index, through the stand-in on sysio.epoch.
   void set_epoch(uint32_t index) {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi_ser, EPOCH_ACCOUNT, "setindex"_n,
                                          mvo()("index", index)));
   }

   fc::variant decode(const char* type, const std::vector<char>& data) {
      return data.empty() ? fc::variant()
                          : andon_abi_ser.binary_to_variant(type, data,
                                                            abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   /// The cord singleton; null before the first pull.
   fc::variant cord() { return decode("cord_state", get_row_by_account(ANDON_ACCOUNT, ANDON_ACCOUNT, "cord"_n, "cord"_n)); }
   /// The configuration singleton; null before the first `setpanic` or `addpuller`.
   fc::variant config() {
      return decode("andon_config", get_row_by_account(ANDON_ACCOUNT, ANDON_ACCOUNT, "andonconfig"_n, "andonconfig"_n));
   }
   /// The cord's raw bytes, for an exact before/after comparison.
   std::vector<char> cord_bytes() { return get_row_by_account(ANDON_ACCOUNT, ANDON_ACCOUNT, "cord"_n, "cord"_n); }

   abi_serializer        andon_abi_ser, epoch_abi_ser;
   transaction_trace_ptr last_trace;
};

BOOST_AUTO_TEST_SUITE(sysio_andon_tests)

// Only sysio configures: it names the panic account, which must exist and can be replaced, and registers
// pullers, each once and at most MAX_PULLERS of them.
BOOST_FIXTURE_TEST_CASE(setpanic_and_addpuller_are_sysio_only, sysio_andon_tester) try {
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), setpanic(PANIC, ALICE));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(NOT_ACCOUNT)), setpanic("nobody"_n));
   BOOST_REQUIRE(config().is_null());
   BOOST_REQUIRE_EQUAL(success(), setpanic(ALICE));
   BOOST_REQUIRE_EQUAL(ALICE, config()["panic"].as<name>());
   BOOST_REQUIRE_EQUAL(success(), setpanic(PANIC));
   BOOST_REQUIRE_EQUAL(PANIC, config()["panic"].as<name>());

   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), addpuller(PULLER, PANIC));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(NOT_ACCOUNT)), addpuller("nobody"_n));
   BOOST_REQUIRE_EQUAL(success(), addpuller(PULLER));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(ALREADY_PULLER)), addpuller(PULLER));
   BOOST_REQUIRE_EQUAL(PANIC, config()["panic"].as<name>());   // a puller leaves the panic account alone
   const auto pullers = config()["pullers"].get_array();
   BOOST_REQUIRE_EQUAL(1u, pullers.size());
   BOOST_REQUIRE_EQUAL(PULLER, pullers[0].as<name>());

   for (uint32_t i = 1; i < MAX_PULLERS; ++i) {
      const name extra{std::string("pullerx") + char('a' + i)};
      create_accounts({extra});
      BOOST_REQUIRE_EQUAL(success(), addpuller(extra));
   }
   BOOST_REQUIRE_EQUAL(MAX_PULLERS, config()["pullers"].get_array().size());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(TOO_MANY_PULLERS)), addpuller(ALICE));
} FC_LOG_AND_RETHROW()

// The panic account, sysio and a registered puller may pull; only the panic account and sysio may clear.
// Anyone else is refused, and naming an authorized actor does not help a signer who is not it.
BOOST_FIXTURE_TEST_CASE(who_may_pull_and_clear, sysio_andon_tester) try {
   // Before any configuration only sysio may act.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(PANIC));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(PULLER));
   BOOST_REQUIRE_EQUAL(success(), setpanic(PANIC));
   BOOST_REQUIRE_EQUAL(success(), addpuller(PULLER));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(ALICE));
   BOOST_REQUIRE_EQUAL(error("missing authority of panic"), pull(PANIC, "incident", ALICE));
   BOOST_REQUIRE(cord().is_null());

   // The panic account pulls; a puller and alice may not clear; the panic account clears.
   BOOST_REQUIRE_EQUAL(success(), pull(PANIC));
   BOOST_REQUIRE(cord()["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(PANIC, cord()["pulled_by"].as<name>());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(CLEAR_AUTHORITY)), clear(PULLER));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(CLEAR_AUTHORITY)), clear(ALICE));
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"), clear(SYSIO_ACCOUNT, "resolved", ALICE));
   BOOST_REQUIRE(cord()["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(success(), clear(PANIC));
   BOOST_REQUIRE(!cord()["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(PANIC, cord()["cleared_by"].as<name>());

   // sysio pulls and clears.
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, cord()["pulled_by"].as<name>());
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, cord()["cleared_by"].as<name>());

   // A registered puller pulls (the path sysio.synd will pull through), and sysio clears.
   BOOST_REQUIRE_EQUAL(success(), pull(PULLER, "custody totals disagree"));
   BOOST_REQUIRE_EQUAL(PULLER, cord()["pulled_by"].as<name>());
   BOOST_REQUIRE_EQUAL("custody totals disagree", cord()["reason"].as_string());
   BOOST_REQUIRE_EQUAL(3u, cord()["pull_count"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT));

   // A replaced panic account loses both powers.
   BOOST_REQUIRE_EQUAL(success(), setpanic(ALICE));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(PANIC));
   BOOST_REQUIRE_EQUAL(success(), pull(ALICE));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(CLEAR_AUTHORITY)), clear(PANIC));
   BOOST_REQUIRE_EQUAL(success(), clear(ALICE));
} FC_LOG_AND_RETHROW()

// Review focus 1: pulling a pulled cord changes nothing and clearing a clear one changes nothing -- only the
// first of each is recorded, and each says so. The cord records the depot epoch of each pull and clear and
// counts every epoch during which it was pulled, both ends included.
BOOST_FIXTURE_TEST_CASE(pulling_twice_and_clearing_twice_change_nothing, sysio_andon_tester) try {
   BOOST_REQUIRE_EQUAL(success(), setpanic(PANIC));
   set_epoch(3);

   // Clearing a cord never pulled changes nothing and writes no row.
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "nothing to clear"));
   BOOST_REQUIRE_NE(std::string::npos, console().find(ALREADY_CLEAR));
   BOOST_REQUIRE(cord().is_null());

   BOOST_REQUIRE_EQUAL(success(), pull(PANIC, "first"));
   const auto first = cord();
   BOOST_REQUIRE(first["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(PANIC, first["pulled_by"].as<name>());
   BOOST_REQUIRE_EQUAL("first", first["reason"].as_string());
   BOOST_REQUIRE_EQUAL(3u, first["pulled_at_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(1u, first["pull_count"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, first["frozen_epochs"].as_uint64());
   const auto pulled_bytes = cord_bytes();

   // A second pull, by another authority, later and for another reason: nothing moves.
   set_epoch(5);
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, "second"));
   BOOST_REQUIRE_NE(std::string::npos, console().find(ALREADY_PULLED));
   BOOST_REQUIRE(pulled_bytes == cord_bytes());

   set_epoch(6);
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "first clear"));
   const auto cleared = cord();
   BOOST_REQUIRE(!cleared["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT, cleared["cleared_by"].as<name>());
   BOOST_REQUIRE_EQUAL("first clear", cleared["note"].as_string());
   BOOST_REQUIRE_EQUAL(6u, cleared["cleared_at_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(4u, cleared["frozen_epochs"].as_uint64());   // epochs 3, 4, 5 and 6
   // The pull is still on record beside the clear.
   BOOST_REQUIRE_EQUAL(PANIC, cleared["pulled_by"].as<name>());
   BOOST_REQUIRE_EQUAL("first", cleared["reason"].as_string());
   const auto cleared_bytes = cord_bytes();

   // A second clear, by the other authority, later: nothing moves.
   set_epoch(7);
   BOOST_REQUIRE_EQUAL(success(), clear(PANIC, "second clear"));
   BOOST_REQUIRE_NE(std::string::npos, console().find(ALREADY_CLEAR));
   BOOST_REQUIRE(cleared_bytes == cord_bytes());

   // The next freeze is counted on top: pulled and cleared in epoch 9, one more frozen epoch.
   set_epoch(9);
   BOOST_REQUIRE_EQUAL(success(), pull(PANIC, "again"));
   BOOST_REQUIRE_EQUAL(2u, cord()["pull_count"].as_uint64());
   BOOST_REQUIRE_EQUAL(9u, cord()["pulled_at_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(4u, cord()["frozen_epochs"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), clear(PANIC, "again"));
   BOOST_REQUIRE_EQUAL(5u, cord()["frozen_epochs"].as_uint64());
   BOOST_REQUIRE_EQUAL(9u, cord()["cleared_at_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// A reason and a note longer than MAX_TEXT_BYTES are cut to it, never refused: a contract pulling inline
// from a path that must not throw cannot be failed by its own reason.
BOOST_FIXTURE_TEST_CASE(reason_and_note_are_cut_to_the_limit, sysio_andon_tester) try {
   const std::string long_text(MAX_TEXT_BYTES + 44, 'x');
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, long_text));
   BOOST_REQUIRE_EQUAL(long_text.substr(0, MAX_TEXT_BYTES), cord()["reason"].as_string());
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, long_text));
   BOOST_REQUIRE_EQUAL(long_text.substr(0, MAX_TEXT_BYTES), cord()["note"].as_string());
} FC_LOG_AND_RETHROW()

// A pull in the epoch the previous freeze was cleared in starts its freeze in the next epoch, and a clear
// in an epoch before that adds nothing: every epoch during which the cord was pulled counts once, however
// often it is pulled and cleared within an epoch.
BOOST_FIXTURE_TEST_CASE(repulls_in_the_clearing_epoch_count_each_epoch_once, sysio_andon_tester) try {
   set_epoch(2);
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, "a"));
   BOOST_REQUIRE_EQUAL(2u, cord()["pulled_at_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "a"));
   BOOST_REQUIRE_EQUAL(1u, cord()["frozen_epochs"].as_uint64());   // epoch 2

   // Pulled and cleared again within epoch 2: epoch 2 is already counted.
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, "b"));
   BOOST_REQUIRE_EQUAL(3u, cord()["pulled_at_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "b"));
   BOOST_REQUIRE_EQUAL(1u, cord()["frozen_epochs"].as_uint64());
   BOOST_REQUIRE_EQUAL(2u, cord()["pull_count"].as_uint64());

   // Pulled again in epoch 2 and cleared in epoch 3: epoch 3 is added.
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, "c"));
   set_epoch(3);
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "c"));
   BOOST_REQUIRE_EQUAL(2u, cord()["frozen_epochs"].as_uint64());   // epochs 2 and 3

   // Across epochs: pulled at 5, cleared at 7 -- epochs 5, 6 and 7.
   set_epoch(5);
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, "d"));
   BOOST_REQUIRE_EQUAL(5u, cord()["pulled_at_epoch"].as<uint32_t>());
   set_epoch(7);
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "d"));
   BOOST_REQUIRE_EQUAL(5u, cord()["frozen_epochs"].as_uint64());

   // Re-pulled in epoch 7 and cleared at 9: epochs 8 and 9 only.
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT, "e"));
   BOOST_REQUIRE_EQUAL(8u, cord()["pulled_at_epoch"].as<uint32_t>());
   set_epoch(9);
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT, "e"));
   BOOST_REQUIRE_EQUAL(7u, cord()["frozen_epochs"].as_uint64());
   BOOST_REQUIRE_EQUAL(5u, cord()["pull_count"].as_uint64());
} FC_LOG_AND_RETHROW()

// `andon::may_pull` is the one pull-authority test -- `pull` refuses exactly the actors it rejects -- and
// Part D's automatic pull checks it before sending `pull`. It admits sysio always, the panic account only
// while it is the panic account, and registered pullers; nobody else.
BOOST_FIXTURE_TEST_CASE(may_pull_admits_sysio_the_panic_account_and_pullers, sysio_andon_tester) try {
   // No configuration row: sysio alone.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(PANIC));
   BOOST_REQUIRE_EQUAL(success(), pull(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), setpanic(PANIC));
   BOOST_REQUIRE_EQUAL(success(), pull(PANIC));             // the panic account
   BOOST_REQUIRE_EQUAL(success(), clear(PANIC));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(PULLER));   // not registered yet
   BOOST_REQUIRE_EQUAL(success(), addpuller(PULLER));
   BOOST_REQUIRE_EQUAL(success(), pull(PULLER));            // a registered puller
   BOOST_REQUIRE_EQUAL(success(), clear(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(ALICE));
   BOOST_REQUIRE_EQUAL(success(), setpanic(ALICE));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(PULL_AUTHORITY)), pull(PANIC));    // replaced
   BOOST_REQUIRE_EQUAL(success(), pull(ALICE));
   BOOST_REQUIRE_EQUAL(4u, cord()["pull_count"].as_uint64());
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
