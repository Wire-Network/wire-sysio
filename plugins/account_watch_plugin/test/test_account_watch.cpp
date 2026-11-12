#include <fc/crypto/hex.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/account_watch_plugin/account_watch.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <savanna_cluster.hpp> // the unit tests' Savanna cluster fixture (see test/CMakeLists.txt)
#include <sstream>
#include <string>
#include <string_view>
#include <test_contracts.hpp> // sysio.token and sysio.wrap, as the build tree holds them
#include <tuple>
#include <vector>

using namespace sysio::chain;
using namespace sysio::testing;
namespace aw = sysio::account_watch;
namespace bpo = boost::program_options;
using mvo = fc::mutable_variant_object;

namespace {

constexpr account_name alice = "alice"_n;
constexpr account_name bob = "bob"_n;
constexpr account_name carol = "carol"_n;
constexpr account_name token_account = "sysio.token"_n;
constexpr account_name wrap_account = "sysio.wrap"_n;
constexpr fc::microseconds abi_max_time = fc::seconds(1);

/** @p command_line, then @p config_ini as the text of a config.ini, through the plugin's options. */
bpo::variables_map parse_options(const std::vector<std::string>& command_line, const std::string& config_ini = {}) {
   bpo::options_description cfg;
   aw::add_options(cfg);
   bpo::variables_map options;
   bpo::store(bpo::command_line_parser(command_line).options(cfg).run(), options);
   std::istringstream ini{config_ini};
   bpo::store(bpo::parse_config_file(ini, cfg), options);
   bpo::notify(options);
   return options;
}

/** A rule parsed as the first --account-watch-include value. */
aw::rule rule_of(std::string_view spec) {
   return aw::parse_rule(spec, aw::option::include, 0);
}

/** A config of @p includes and @p excludes, with no heartbeat unless asked for. */
aw::config config_of(const std::vector<std::string_view>& includes, const std::vector<std::string_view>& excludes = {},
                     uint32_t max_data_bytes = aw::default_max_data_bytes, uint32_t heartbeat_seconds = 0,
                     const std::vector<std::string_view>& receivers = {}) {
   std::vector<aw::rule> include_rules;
   for (std::size_t i = 0; i < includes.size(); ++i)
      include_rules.push_back(aw::parse_rule(includes[i], aw::option::include, i));
   std::vector<aw::rule> exclude_rules;
   for (std::size_t i = 0; i < excludes.size(); ++i)
      exclude_rules.push_back(aw::parse_rule(excludes[i], aw::option::exclude, i));
   std::vector<aw::receiver_rule> receiver_rules;
   for (std::size_t i = 0; i < receivers.size(); ++i)
      receiver_rules.push_back(aw::parse_receiver_rule(receivers[i], aw::option::receiver, i));
   return aw::config{
      .rules = aw::rule_set(std::move(include_rules), std::move(exclude_rules), std::move(receiver_rules)),
      .max_data_bytes = max_data_bytes,
      .heartbeat_interval = fc::seconds(heartbeat_seconds),
   };
}

/** An action of @p contract named @p name with @p authorization and no data. */
action action_of(std::vector<permission_level> authorization, account_name contract, action_name name) {
   action act;
   act.account = contract;
   act.name = name;
   act.authorization = std::move(authorization);
   return act;
}

/** A predicate for BOOST_CHECK_EXCEPTION: the exception's message contains every one of @p parts. */
auto message_contains(std::vector<std::string> parts) {
   return [parts = std::move(parts)](const fc::exception& e) {
      const std::string message = e.top_message();
      for (const std::string& part : parts) {
         if (message.find(part) == std::string::npos) {
            BOOST_TEST_MESSAGE("'" << part << "' not in: " << message);
            return false;
         }
      }
      return true;
   };
}

/// One record a watcher reported.
struct captured {
   aw::event kind;
   fc::variant_object record;
};

/// A watcher attached to one controller's signals, recording what it reports, timed by a clock the test sets.
class watch_harness {
public:
   ~watch_harness() { detach(); }

   /**
    * Replace any attached watcher with one of @p cfg on @p chain, decoding for at most @p decode_time per block, and
    * forget what the old one reported.
    */
   void attach(controller& chain, aw::config cfg, fc::microseconds decode_time = abi_max_time) {
      detach();
      records.clear();
      watch.emplace(
         std::move(cfg), chain, decode_time,
         [this](aw::event kind, const fc::variant_object& record) { records.push_back({kind, record}); },
         [this] { return now; });
      block_start.emplace(
         chain.block_start().connect([this](uint32_t block_num) { watch->on_block_start(block_num); }));
      applied_transaction.emplace(chain.applied_transaction().connect(
         [this](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
            watch->on_applied_transaction(std::get<0>(t));
         }));
      accepted_block.emplace(chain.accepted_block().connect(
         [this](const block_signal_params& t) { watch->on_accepted_block(std::get<0>(t), std::get<1>(t)); }));
   }

   /** Disconnect and drop the watcher. */
   void detach() {
      accepted_block.reset();
      applied_transaction.reset();
      block_start.reset();
      watch.reset();
   }

   /** The records of @p kind, in order. */
   std::vector<fc::variant_object> of(aw::event kind) const {
      std::vector<fc::variant_object> out;
      for (const captured& c : records)
         if (c.kind == kind)
            out.push_back(c.record);
      return out;
   }

   std::vector<captured> records;
   fc::time_point now = fc::time_point::from_iso_string("2026-11-11T00:00:00");
   std::optional<aw::watcher> watch;

private:
   std::optional<boost::signals2::scoped_connection> block_start;
   std::optional<boost::signals2::scoped_connection> applied_transaction;
   std::optional<boost::signals2::scoped_connection> accepted_block;
};

/// A chain with sysio.token deployed, alice funded and bob holding some tokens, and a watch harness for it.
struct chain_fixture {
   tester chain;
   watch_harness harness; // declared after the chain, so it disconnects before the chain closes

   chain_fixture() {
      chain.create_accounts({alice, bob, carol, token_account});
      chain.set_code(token_account, test_contracts::sysio_token_wasm());
      chain.set_abi(token_account, test_contracts::sysio_token_abi().c_str());
      chain.set_privileged(token_account);
      chain.produce_block();
      chain.push_action(token_account, "create"_n, token_account,
                        mvo()("issuer", alice)("maximum_supply", "1000000.0000 TOK"));
      chain.push_action(token_account, "issue"_n, alice, mvo()("to", alice)("quantity", "1000.0000 TOK")("memo", ""));
      transfer(alice, bob, "100.0000 TOK");
      chain.produce_block();
   }

   /** Watch with @p cfg from here on, decoding for at most @p decode_time per block. */
   void watch(aw::config cfg, fc::microseconds decode_time = abi_max_time) {
      harness.attach(*chain.control, std::move(cfg), decode_time);
   }

   /** Push carol::create with @p data, authorized by carol, and produce the block. */
   void push_carol_create(const bytes& data) {
      signed_transaction trx;
      trx.actions.emplace_back(
         std::vector<permission_level>{
            {carol, config::active_name}
      },
         carol, "create"_n, data);
      chain.set_transaction_headers(trx);
      trx.sign(tester::get_private_key(carol, "active"), chain.control->get_chain_id());
      chain.push_transaction(trx);
      chain.produce_block();
   }

   /** Transfer @p quantity from @p from to @p to, authorized by @p authorization (from@active by default). */
   transaction_trace_ptr transfer(account_name from, account_name to, std::string_view quantity,
                                  std::optional<std::vector<permission_level>> authorization = std::nullopt) {
      return chain.push_action(token_account, "transfer"_n,
                               authorization.value_or(std::vector<permission_level>{
                                  {from, config::active_name}
      }),
                               mvo()("from", from)("to", to)("quantity", std::string{quantity})("memo", "m"));
   }

   std::vector<fc::variant_object> matches() const { return harness.of(aw::event::match); }
};

/** The `permission_level` array a record holds under @p key. */
std::vector<permission_level> levels(const fc::variant_object& record, const std::string& key) {
   return record[key].as<std::vector<permission_level>>();
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(account_watch_rule_tests)

BOOST_AUTO_TEST_CASE(parse_rule_fields_and_wildcards) {
   const aw::rule all_of_actor = rule_of("prod.a,*::*");
   BOOST_CHECK(all_of_actor.actor == "prod.a"_n);
   BOOST_CHECK(!all_of_actor.permission);
   BOOST_CHECK(!all_of_actor.contract);
   BOOST_CHECK(!all_of_actor.action);
   BOOST_CHECK_EQUAL(all_of_actor.to_string(), "prod.a@*,*::*");

   const aw::rule exact = rule_of("snap.a@active,sysio::votesnaphash");
   BOOST_CHECK(exact.actor == "snap.a"_n);
   BOOST_CHECK(exact.permission == config::active_name);
   BOOST_CHECK(exact.contract == config::system_account_name);
   BOOST_CHECK(exact.action == "votesnaphash"_n);
   BOOST_CHECK_EQUAL(exact.to_string(), "snap.a@active,sysio::votesnaphash");

   const aw::rule any_actor = rule_of("*,sysio::setcode");
   BOOST_CHECK(!any_actor.actor);
   BOOST_CHECK(!any_actor.permission);
   BOOST_CHECK_EQUAL(any_actor.to_string(), "*@*,sysio::setcode");

   const aw::rule any_owner = rule_of("*@owner,*::*");
   BOOST_CHECK(!any_owner.actor);
   BOOST_CHECK(any_owner.permission == config::owner_name);

   // to_string is itself a valid spec that parses back to the same rule.
   for (const std::string_view spec : {"prod.a,*::*", "snap.a@active,sysio::votesnaphash", "*@owner,sysio.token::*"}) {
      const aw::rule parsed = rule_of(spec);
      const aw::rule reparsed = rule_of(parsed.to_string());
      BOOST_CHECK(reparsed.actor == parsed.actor);
      BOOST_CHECK(reparsed.permission == parsed.permission);
      BOOST_CHECK(reparsed.contract == parsed.contract);
      BOOST_CHECK(reparsed.action == parsed.action);
   }
}

BOOST_AUTO_TEST_CASE(parse_rule_rejects_malformed_values) {
   const auto rejects = [](std::string_view spec, const std::string& problem) {
      BOOST_TEST_CONTEXT(spec) {
         BOOST_CHECK_EXCEPTION(aw::parse_rule(spec, aw::option::exclude, 3), plugin_config_exception,
                               message_contains({fmt::format("--{} value 3 '{}'", aw::option::exclude, spec), problem,
                                                 std::string{aw::rule_format}}));
      }
   };
   rejects("prod.a", "expected exactly one ','");
   rejects("prod.a,sysio::a,b", "expected exactly one ','");
   rejects("prod.a@active@owner,*::*", "expected at most one '@'");
   rejects("prod.a,sysio", "the action part needs '::'");
   rejects("prod.a,", "the action part needs '::'");
   rejects(",*::*", "the actor is empty");
   rejects("prod.a@,*::*", "the permission is empty");
   rejects("prod.a,::transfer", "the contract is empty");
   rejects("prod.a,sysio::", "the action is empty");
   rejects("Prod.a,*::*", "the actor is not a valid name");
   rejects("prod a,*::*", "the actor is not a valid name");
   rejects("prod.a@toolongpermission,*::*", "the permission is not a valid name");
   rejects("prod.a,sysio.token.x.y.z::*", "the contract is not a valid name");
   rejects("prod.a,sysio::a::b", "the action is not a valid name");
   rejects("prod.a, sysio::transfer", "the contract is not a valid name");
   rejects("prod.*,*::*", "the actor is not a valid name");
}

BOOST_AUTO_TEST_CASE(include_minus_exclude_per_authorization) {
   const aw::config cfg =
      config_of({"prod.a,*::*", "snap.a,*::*", "*,sysio::setcode"}, {"snap.a@active,sysio::votesnaphash"});
   const permission_level prod_active{"prod.a"_n, config::active_name};
   const permission_level snap_active{"snap.a"_n, config::active_name};
   const permission_level snap_owner{"snap.a"_n, config::owner_name};
   const permission_level carol_active{carol, config::active_name};
   using levels_t = std::vector<permission_level>;
   const auto matched = [&](levels_t authorization, account_name contract, action_name name) {
      return cfg.rules.matched_authorizations(action_of(std::move(authorization), contract, name));
   };

   BOOST_CHECK(matched({prod_active}, config::system_account_name, "updateauth"_n) == levels_t{prod_active});
   BOOST_CHECK(matched({snap_owner}, config::system_account_name, "votesnaphash"_n) == levels_t{snap_owner});
   BOOST_CHECK(matched({snap_active}, config::system_account_name, "votesnaphash"_n).empty());
   // The exclude covers snap.a only; prod.a co-signing the same action is still reported.
   BOOST_CHECK(matched({snap_active, prod_active}, config::system_account_name, "votesnaphash"_n) ==
               levels_t{prod_active});
   BOOST_CHECK(matched({carol_active}, config::system_account_name, "setcode"_n) == levels_t{carol_active});
   BOOST_CHECK(matched({carol_active}, token_account, "transfer"_n).empty());
   // A context-free action has no authorization, so nothing can match it.
   BOOST_CHECK(matched({}, config::system_account_name, "setcode"_n).empty());
}

BOOST_AUTO_TEST_CASE(receiver_rule_parsing_and_matching) {
   const aw::receiver_rule transfers = aw::parse_receiver_rule("alice,sysio.token::transfer", aw::option::receiver, 0);
   BOOST_CHECK(transfers.receiver == alice);
   BOOST_CHECK(transfers.contract == token_account);
   BOOST_CHECK(transfers.action == "transfer"_n);
   BOOST_CHECK_EQUAL(transfers.to_string(), "alice,sysio.token::transfer");
   BOOST_CHECK_EQUAL(aw::parse_receiver_rule("*,*::*", aw::option::receiver, 0).to_string(), "*,*::*");

   const action transfer_act = action_of(
      {
         {bob, config::active_name}
   },
      token_account, "transfer"_n);
   BOOST_CHECK(transfers.matches(alice, transfer_act));
   BOOST_CHECK(!transfers.matches(bob, transfer_act));
   BOOST_CHECK(!transfers.matches(alice, action_of(
                                            {
                                               {bob, config::active_name}
   },
                                            token_account, "issue"_n)));

   BOOST_CHECK(!aw::rule_set().matches_receiver(alice, transfer_act));
   BOOST_CHECK(aw::rule_set({}, {}, {transfers}).matches_receiver(alice, transfer_act));
   // A wildcard receiver matches every receiver.
   const aw::rule_set any_receiver({}, {},
                                   {aw::parse_receiver_rule("*,sysio.token::transfer", aw::option::receiver, 0)});
   BOOST_CHECK(any_receiver.matches_receiver(alice, transfer_act));
   BOOST_CHECK(any_receiver.matches_receiver(bob, transfer_act));
   // Excludes never apply to receiver rules.
   BOOST_CHECK(aw::rule_set({}, {rule_of("bob,*::*")}, {transfers}).matches_receiver(alice, transfer_act));

   const auto rejects = [](std::string_view spec, const std::string& problem) {
      BOOST_TEST_CONTEXT(spec) {
         BOOST_CHECK_EXCEPTION(aw::parse_receiver_rule(spec, aw::option::receiver, 2), plugin_config_exception,
                               message_contains({fmt::format("--{} value 2 '{}'", aw::option::receiver, spec), problem,
                                                 std::string{aw::receiver_rule_format}}));
      }
   };
   rejects("alice", "expected exactly one ','");
   rejects("alice@active,sysio.token::transfer", "the receiver is not a valid name");
   rejects(",sysio.token::transfer", "the receiver is empty");
   rejects("alice,sysio.token", "the action part needs '::'");
   rejects("alice,::transfer", "the contract is empty");
}

BOOST_AUTO_TEST_CASE(explicit_payer_matched_only_alone) {
   const permission_level payer{alice, config::sysio_payer_name};
   const permission_level active{alice, config::active_name};
   const action paired = action_of({payer, active}, token_account, "transfer"_n);

   BOOST_CHECK(config_of({"alice,*::*"}).rules.matched_authorizations(paired) == std::vector<permission_level>{active});
   // Excluding alice@active silences the action: the payer entry beside it cannot report it.
   BOOST_CHECK(
      config_of({"alice,*::*"}, {"alice@active,sysio.token::transfer"}).rules.matched_authorizations(paired).empty());
   BOOST_CHECK(config_of({"alice@sysio.payer,*::*"}).rules.matched_authorizations(paired).empty());
   // A payer beside another actor's authorization is still skipped only for its own actor.
   const permission_level bob_active{bob, config::active_name};
   BOOST_CHECK(config_of({"alice,*::*"})
                  .rules.matched_authorizations(action_of({payer, bob_active}, token_account, "transfer"_n)) ==
               std::vector<permission_level>{payer});

   // Alone, which only a privileged contract's inline action can be, the payer is alice's authority.
   const action alone = action_of({payer}, token_account, "transfer"_n);
   BOOST_CHECK(config_of({"alice,*::*"}).rules.matched_authorizations(alone) == std::vector<permission_level>{payer});
   BOOST_CHECK(config_of({"alice,*::*"}, {"alice@active,sysio.token::transfer"}).rules.matched_authorizations(alone) ==
               std::vector<permission_level>{payer});
}

BOOST_AUTO_TEST_CASE(parse_config_options) {
   // An enabled plugin with no include rule fails startup, with or without excludes.
   const std::string rule_required =
      fmt::format("at least one --{} or --{} is required", aw::option::include, aw::option::receiver);
   BOOST_CHECK_EXCEPTION(aw::parse_config(parse_options({})), plugin_config_exception,
                         message_contains({rule_required}));
   BOOST_CHECK_EXCEPTION(aw::parse_config(parse_options({fmt::format("--{}=alice,*::*", aw::option::exclude)})),
                         plugin_config_exception, message_contains({rule_required}));
   // Excludes apply only to include rules.
   BOOST_CHECK_EXCEPTION(
      aw::parse_config(parse_options(
         {fmt::format("--{}=alice,*::*", aw::option::exclude), fmt::format("--{}=alice,*::*", aw::option::receiver)})),
      plugin_config_exception,
      message_contains({fmt::format("--{} applies only to --{} rules", aw::option::exclude, aw::option::include)}));

   // Receiver rules alone are enough.
   const aw::config receivers_only =
      aw::parse_config(parse_options({fmt::format("--{}=alice,sysio.token::transfer", aw::option::receiver)}));
   BOOST_CHECK(receivers_only.rules.includes().empty());
   BOOST_REQUIRE_EQUAL(receivers_only.rules.receivers().size(), 1u);
   BOOST_CHECK_EQUAL(receivers_only.rules.receivers()[0].to_string(), "alice,sysio.token::transfer");

   const aw::config defaults = aw::parse_config(parse_options({fmt::format("--{}=alice,*::*", aw::option::include)}));
   BOOST_REQUIRE_EQUAL(defaults.rules.includes().size(), 1u);
   BOOST_CHECK(defaults.rules.excludes().empty());
   BOOST_CHECK_EQUAL(defaults.max_data_bytes, aw::default_max_data_bytes);
   BOOST_CHECK_EQUAL(defaults.heartbeat_interval.count(), fc::seconds(aw::default_heartbeat_seconds).count());

   // Command-line and config.ini values compose, command line first; a scalar given on the command line wins.
   const aw::config composed = aw::parse_config(parse_options(
      {fmt::format("--{}=alice,*::*", aw::option::include), fmt::format("--{}=0", aw::option::heartbeat_seconds)},
      fmt::format("{0} = bob@owner,*::*\n{1} = alice@active,sysio::reqauth\n{2} = 64\n{3} = 30\n", aw::option::include,
                  aw::option::exclude, aw::option::max_data_bytes, aw::option::heartbeat_seconds)));
   BOOST_REQUIRE_EQUAL(composed.rules.includes().size(), 2u);
   BOOST_CHECK_EQUAL(composed.rules.includes()[0].to_string(), "alice@*,*::*");
   BOOST_CHECK_EQUAL(composed.rules.includes()[1].to_string(), "bob@owner,*::*");
   BOOST_REQUIRE_EQUAL(composed.rules.excludes().size(), 1u);
   BOOST_CHECK_EQUAL(composed.rules.excludes()[0].to_string(), "alice@active,sysio::reqauth");
   BOOST_CHECK_EQUAL(composed.max_data_bytes, 64u);
   BOOST_CHECK_EQUAL(composed.heartbeat_interval.count(), 0);

   // A bad config.ini value is named by its position among all of the option's values.
   BOOST_CHECK_EXCEPTION(aw::parse_config(parse_options({fmt::format("--{}=alice,*::*", aw::option::include)},
                                                        fmt::format("{} = bob\n", aw::option::include))),
                         plugin_config_exception,
                         message_contains({fmt::format("--{} value 1 'bob'", aw::option::include)}));
}

BOOST_AUTO_TEST_CASE(json_line_is_one_parseable_line) {
   const fc::variant_object record = mvo()(aw::field::event, "match")(aw::field::data, mvo()("memo", "two\nlines"));
   const std::string line = aw::to_json_line(record);
   BOOST_CHECK_EQUAL(line.find('\n'), std::string::npos);
   const fc::variant parsed = fc::json::from_string(line);
   BOOST_CHECK_EQUAL(parsed.get_object()[aw::field::event].as_string(), "match");
   BOOST_CHECK_EQUAL(parsed.get_object()[aw::field::data]["memo"].as_string(), "two\nlines");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(account_watch_chain_tests)

BOOST_FIXTURE_TEST_CASE(reports_authorized_action_once_at_accepted_block, chain_fixture) {
   watch(config_of({"alice,*::*"}));
   const transaction_trace_ptr trace = transfer(alice, bob, "1.0000 TOK");
   // Held until its block is accepted.
   BOOST_CHECK(matches().empty());
   const signed_block_ptr block = chain.produce_block();

   // sysio.token notifies alice and bob; only the action's own execution is reported.
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
   const std::vector<fc::variant_object> found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   const fc::variant_object& m = found[0];
   const action_trace& at = trace->action_traces[0];
   BOOST_CHECK_EQUAL(m[aw::field::event].as_string(), "match");
   BOOST_CHECK_EQUAL(m[aw::field::block_num].as_uint64(), block->block_num());
   BOOST_CHECK_EQUAL(m[aw::field::block_id].as_string(), block->calculate_id().str());
   BOOST_CHECK_EQUAL(m[aw::field::block_time].as_string(), fc::variant{block->timestamp}.as_string());
   BOOST_CHECK_EQUAL(m[aw::field::producer].as_string(), block->producer.to_string());
   BOOST_CHECK_LE(m[aw::field::lib_num].as_uint64(), block->block_num());
   BOOST_CHECK_GT(m[aw::field::lib_num].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(m[aw::field::trx_id].as_string(), trace->id.str());
   BOOST_CHECK_EQUAL(m[aw::field::action_ordinal].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(m[aw::field::global_sequence].as_string(), std::to_string(at.receipt->global_sequence));
   BOOST_CHECK_EQUAL(m[aw::field::contract].as_string(), token_account.to_string());
   BOOST_CHECK_EQUAL(m[aw::field::action].as_string(), "transfer");
   BOOST_CHECK_EQUAL(m[aw::field::receiver].as_string(), token_account.to_string());
   const std::vector<permission_level> alice_active{
      {alice, config::active_name}
   };
   BOOST_CHECK(levels(m, aw::field::authorization) == alice_active);
   BOOST_CHECK(levels(m, aw::field::matched) == alice_active);
   BOOST_CHECK_EQUAL(m[aw::field::data_size].as_uint64(), at.act.data.size());
   BOOST_REQUIRE(m.contains(aw::field::data));
   BOOST_CHECK_EQUAL(m[aw::field::data]["quantity"].as_string(), "1.0000 TOK");
   BOOST_CHECK_EQUAL(m[aw::field::data]["to"].as_string(), "bob");
   BOOST_CHECK(!m.contains(aw::field::data_hex));
   BOOST_CHECK(!m.contains(aw::field::data_sha256));
   BOOST_CHECK_EQUAL(harness.watch->matches_reported(), 1u);

   // The JSON line keeps global_sequence a string and parses back to the same record.
   const fc::variant line = fc::json::from_string(aw::to_json_line(m));
   BOOST_CHECK(line.get_object()[aw::field::global_sequence].is_string());
   BOOST_CHECK_EQUAL(line.get_object()[aw::field::trx_id].as_string(), trace->id.str());

   // The next block reports nothing new.
   chain.produce_block();
   BOOST_CHECK_EQUAL(matches().size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(receiver_rules_report_notifications, chain_fixture) {
   watch(config_of({}, {}, aw::default_max_data_bytes, 0, {"alice,sysio.token::transfer"}));

   // alice receives: the notification to alice is reported, not the execution or bob's notification.
   const transaction_trace_ptr received = transfer(bob, alice, "1.0000 TOK");
   transfer(bob, carol, "1.0000 TOK");
   chain.produce_block();
   std::vector<fc::variant_object> found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   const auto notified_alice =
      std::ranges::find_if(received->action_traces, [](const action_trace& at) { return at.receiver == alice; });
   BOOST_REQUIRE(notified_alice != received->action_traces.end());
   BOOST_CHECK_EQUAL(found[0][aw::field::trx_id].as_string(), received->id.str());
   BOOST_CHECK_EQUAL(found[0][aw::field::receiver].as_string(), "alice");
   BOOST_CHECK_EQUAL(found[0][aw::field::contract].as_string(), token_account.to_string());
   BOOST_CHECK_EQUAL(found[0][aw::field::action_ordinal].as_uint64(),
                     static_cast<uint32_t>(notified_alice->action_ordinal));
   BOOST_CHECK_EQUAL(found[0][aw::field::global_sequence].as_string(),
                     std::to_string(notified_alice->receipt->global_sequence));
   BOOST_CHECK(levels(found[0], aw::field::matched).empty());
   BOOST_CHECK(levels(found[0], aw::field::authorization) == (std::vector<permission_level>{
                                                                {bob, config::active_name}
   }));
   BOOST_CHECK_EQUAL(found[0][aw::field::data]["to"].as_string(), "alice");

   // alice sends: sysio.token notifies the sender too.
   transfer(alice, bob, "1.0000 TOK");
   chain.produce_block();
   found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 2u);
   BOOST_CHECK_EQUAL(found[1][aw::field::receiver].as_string(), "alice");
   BOOST_CHECK_EQUAL(found[1][aw::field::data]["from"].as_string(), "alice");

   // With an include rule as well, alice's transfer is reported at its execution (her authorization) and at her
   // notification (the receiver rule): two records, told apart by receiver and action_ordinal.
   watch(config_of({"alice,*::*"}, {}, aw::default_max_data_bytes, 0, {"alice,sysio.token::transfer"}));
   transfer(alice, bob, "1.0000 TOK");
   chain.produce_block();
   found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 2u);
   BOOST_CHECK_EQUAL(found[0][aw::field::receiver].as_string(), token_account.to_string());
   BOOST_CHECK(levels(found[0], aw::field::matched) == (std::vector<permission_level>{
                                                          {alice, config::active_name}
   }));
   BOOST_CHECK_EQUAL(found[1][aw::field::receiver].as_string(), "alice");
   BOOST_CHECK(levels(found[1], aw::field::matched).empty());
   BOOST_CHECK_NE(found[0][aw::field::action_ordinal].as_uint64(), found[1][aw::field::action_ordinal].as_uint64());

   // A receiver rule naming the contract matches the action's own execution.
   watch(config_of({}, {}, aw::default_max_data_bytes, 0, {"sysio.token,*::transfer"}));
   transfer(bob, carol, "1.0000 TOK");
   chain.produce_block();
   found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   BOOST_CHECK_EQUAL(found[0][aw::field::receiver].as_string(), token_account.to_string());
   BOOST_CHECK_EQUAL(found[0][aw::field::action_ordinal].as_uint64(), 1u);
}

BOOST_FIXTURE_TEST_CASE(notified_or_unwatched_accounts_are_not_reported, chain_fixture) {
   watch(config_of({"alice,*::*"}));
   transfer(bob, alice, "1.0000 TOK"); // alice is only notified
   transfer(bob, carol, "1.0000 TOK");
   chain.produce_block();
   BOOST_CHECK(matches().empty());
}

BOOST_FIXTURE_TEST_CASE(dropped_execution_is_not_reported_until_it_lands, chain_fixture) {
   watch(config_of({"alice,*::*"}));
   const transaction_trace_ptr trace = transfer(alice, bob, "1.0000 TOK");
   // The pending block that executed the transfer is aborted and replaced by one without it.
   const signed_block_ptr empty = chain.produce_empty_block();
   BOOST_CHECK(matches().empty());
   // Re-executed into the next block, it is reported there.
   const signed_block_ptr landed = chain.produce_block();
   const std::vector<fc::variant_object> found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   BOOST_CHECK_EQUAL(found[0][aw::field::trx_id].as_string(), trace->id.str());
   BOOST_CHECK_EQUAL(found[0][aw::field::block_num].as_uint64(), landed->block_num());
   BOOST_CHECK_NE(found[0][aw::field::block_num].as_uint64(), empty->block_num());
}

BOOST_FIXTURE_TEST_CASE(block_start_drops_held_matches, chain_fixture) {
   watch(config_of({"alice,*::*"}));
   transfer(alice, bob, "1.0000 TOK");
   // The transfer is already in the pending block, so finishing it does not execute the transfer again: once a
   // block_start has dropped the hold, nothing is left to report.
   harness.watch->on_block_start(chain.control->head().block_num() + 1);
   chain.produce_block();
   BOOST_CHECK(matches().empty());
}

BOOST_FIXTURE_TEST_CASE(failed_transaction_is_not_reported, chain_fixture) {
   watch(config_of({"alice,*::*"}));
   BOOST_CHECK_THROW(transfer(alice, bob, "100000.0000 TOK"), fc::exception);
   chain.produce_block();
   BOOST_CHECK(matches().empty());
}

BOOST_FIXTURE_TEST_CASE(implicit_onblock_is_never_reported, chain_fixture) {
   // Both rules match onblock's sysio@active authorization; onblock is not a signed transaction.
   watch(config_of({"sysio,*::*", "*,*::*"}));
   chain.produce_blocks(3);
   BOOST_CHECK(matches().empty());
}

BOOST_FIXTURE_TEST_CASE(explicit_payer_does_not_defeat_an_exclude, chain_fixture) {
   const std::vector<permission_level> paid{
      {alice, config::sysio_payer_name},
      {alice, config::active_name     }
   };
   watch(config_of({"alice,*::*"}, {"alice@active,sysio.token::transfer"}));
   transfer(alice, bob, "1.0000 TOK", paid);
   chain.produce_block();
   BOOST_CHECK(matches().empty());

   watch(config_of({"alice,*::*"}));
   transfer(alice, bob, "1.0000 TOK", paid);
   chain.produce_block();
   const std::vector<fc::variant_object> found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   BOOST_CHECK(levels(found[0], aw::field::authorization) == paid);
   BOOST_CHECK((levels(found[0], aw::field::matched) == std::vector<permission_level>{
                                                           {alice, config::active_name}
   }));
}

BOOST_FIXTURE_TEST_CASE(inline_action_is_reported, chain_fixture) {
   // sysio.wrap is privileged: exec runs a wrapped transaction's actions inline, with their declared authorizations.
   chain.create_accounts({wrap_account});
   chain.set_code(wrap_account, test_contracts::sysio_wrap_wasm());
   chain.set_abi(wrap_account, test_contracts::sysio_wrap_abi().c_str());
   chain.set_privileged(wrap_account);
   chain.produce_block();

   transaction wrapped;
   chain.set_transaction_headers(wrapped);
   wrapped.actions.emplace_back(
      std::vector<permission_level>{
         {alice, config::active_name}
   },
      config::system_account_name, "reqauth"_n, fc::raw::pack(alice));
   signed_transaction outer;
   chain.set_transaction_headers(outer);
   outer.actions.push_back(chain.get_action(
      wrap_account, "exec"_n,
      {
         {bob,          config::sysio_payer_name},
         {bob,          config::active_name     },
         {wrap_account, config::active_name     }
   },
      mvo()("executer", bob)("trx", wrapped)));
   outer.sign(tester::get_private_key(bob, "active"), chain.control->get_chain_id());
   outer.sign(tester::get_private_key(wrap_account, "active"), chain.control->get_chain_id());

   watch(config_of({"alice,*::*"}));
   const transaction_trace_ptr trace = chain.push_transaction(outer);
   chain.produce_block();

   // alice never signed: the inline reqauth carries her authorization, the exec around it does not.
   const std::vector<fc::variant_object> found = matches();
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   BOOST_CHECK_EQUAL(found[0][aw::field::trx_id].as_string(), trace->id.str());
   BOOST_CHECK_EQUAL(found[0][aw::field::contract].as_string(), config::system_account_name.to_string());
   BOOST_CHECK_EQUAL(found[0][aw::field::action].as_string(), "reqauth");
   BOOST_CHECK_EQUAL(found[0][aw::field::action_ordinal].as_uint64(), 2u);
   BOOST_CHECK_EQUAL(found[0][aw::field::data]["from"].as_string(), "alice");
}

BOOST_FIXTURE_TEST_CASE(data_is_decoded_hashed_or_hex, chain_fixture) {
   watch(config_of({"alice,*::*"}));
   const transaction_trace_ptr first = transfer(alice, bob, "1.0000 TOK");
   chain.produce_block();
   const bytes& data = first->action_traces[0].act.data;
   const uint32_t size = static_cast<uint32_t>(data.size());

   // Exactly at the limit, the data is decoded.
   watch(config_of({"alice,*::*"}, {}, size));
   transfer(alice, bob, "2.0000 TOK");
   chain.produce_block();
   BOOST_REQUIRE_EQUAL(matches().size(), 1u);
   BOOST_CHECK_EQUAL(matches()[0][aw::field::data]["quantity"].as_string(), "2.0000 TOK");

   // One byte over, it is hashed and nothing is decoded.
   watch(config_of({"alice,*::*"}, {}, size - 1));
   const transaction_trace_ptr hashed = transfer(alice, bob, "3.0000 TOK");
   chain.produce_block();
   BOOST_REQUIRE_EQUAL(matches().size(), 1u);
   const fc::variant_object over = matches()[0];
   const bytes& hashed_data = hashed->action_traces[0].act.data;
   BOOST_CHECK_EQUAL(over[aw::field::data_size].as_uint64(), hashed_data.size());
   BOOST_CHECK_EQUAL(over[aw::field::data_sha256].as_string(),
                     fc::sha256::hash(hashed_data.data(), hashed_data.size()).str());
   BOOST_CHECK(!over.contains(aw::field::data));
   BOOST_CHECK(!over.contains(aw::field::data_hex));

   // Once the block's decode time is spent, the data is hex although the ABI is there.
   watch(config_of({"alice,*::*"}), fc::microseconds{0});
   const transaction_trace_ptr unbudgeted = transfer(alice, bob, "4.0000 TOK");
   chain.produce_block();
   BOOST_REQUIRE_EQUAL(matches().size(), 1u);
   const bytes& unbudgeted_data = unbudgeted->action_traces[0].act.data;
   BOOST_CHECK_EQUAL(matches()[0][aw::field::data_hex].as_string(),
                     fc::to_hex(unbudgeted_data.data(), unbudgeted_data.size()));
   BOOST_CHECK(!matches()[0].contains(aw::field::data));
}

BOOST_FIXTURE_TEST_CASE(undecodable_data_is_hex, chain_fixture) {
   // carol runs the token contract under ABIs that cannot decode its create action.
   chain.set_code(carol, test_contracts::sysio_token_wasm());
   chain.set_privileged(carol); // as sysio.token is: its tables bill sysio
   chain.produce_block();
   const bytes create_data = fc::raw::pack(std::make_tuple(carol, asset::from_string("1000.0000 XYZ")));
   const std::string hex = fc::to_hex(create_data.data(), create_data.size());
   const auto check_hex = [&](const fc::variant_object& m) {
      BOOST_CHECK_EQUAL(m[aw::field::data_size].as_uint64(), create_data.size());
      BOOST_CHECK_EQUAL(m[aw::field::data_hex].as_string(), hex);
      BOOST_CHECK(!m.contains(aw::field::data));
      BOOST_CHECK(!m.contains(aw::field::data_sha256));
   };

   // No ABI at all.
   watch(config_of({"carol,*::*"}));
   push_carol_create(create_data);
   BOOST_REQUIRE_EQUAL(matches().size(), 1u);
   check_hex(matches()[0]);

   // An ABI without the action.
   chain.set_abi(carol, R"({"version":"sysio::abi/1.2","structs":[],"actions":[]})");
   chain.produce_block();
   watch(config_of({"carol,*::*"}));
   push_carol_create(fc::raw::pack(std::make_tuple(carol, asset::from_string("1000.0000 XYY"))));
   BOOST_REQUIRE_EQUAL(matches().size(), 1u);
   BOOST_CHECK(!matches()[0].contains(aw::field::data));
   BOOST_CHECK(matches()[0].contains(aw::field::data_hex));

   // An ABI whose action type wants more data than the action carries: the decode fails.
   chain.set_abi(carol, R"({"version":"sysio::abi/1.2",
      "structs":[{"name":"create","base":"","fields":[{"name":"issuer","type":"name"},
         {"name":"maximum_supply","type":"asset"},{"name":"extra","type":"uint64"}]}],
      "actions":[{"name":"create","type":"create","ricardian_contract":""}]})");
   chain.produce_block();
   watch(config_of({"carol,*::*"}));
   const bytes short_data = fc::raw::pack(std::make_tuple(carol, asset::from_string("1000.0000 XYX")));
   push_carol_create(short_data);
   BOOST_REQUIRE_EQUAL(matches().size(), 1u);
   BOOST_CHECK_EQUAL(matches()[0][aw::field::data_hex].as_string(), fc::to_hex(short_data.data(), short_data.size()));
   BOOST_CHECK(!matches()[0].contains(aw::field::data));
}

BOOST_FIXTURE_TEST_CASE(heartbeat_by_wall_clock, chain_fixture) {
   watch(config_of({"alice,*::*"}, {}, aw::default_max_data_bytes, 10));
   // The first accepted block always carries one.
   const signed_block_ptr first = chain.produce_block();
   BOOST_REQUIRE_EQUAL(harness.of(aw::event::heartbeat).size(), 1u);
   harness.now += fc::seconds(9);
   transfer(alice, bob, "1.0000 TOK");
   chain.produce_block();
   BOOST_CHECK_EQUAL(harness.of(aw::event::heartbeat).size(), 1u);
   harness.now += fc::seconds(1);
   const signed_block_ptr due = chain.produce_block();
   const std::vector<fc::variant_object> beats = harness.of(aw::event::heartbeat);
   BOOST_REQUIRE_EQUAL(beats.size(), 2u);
   BOOST_CHECK_EQUAL(beats[0][aw::field::block_num].as_uint64(), first->block_num());
   BOOST_CHECK_EQUAL(beats[0][aw::field::matches].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(beats[1][aw::field::block_num].as_uint64(), due->block_num());
   BOOST_CHECK_EQUAL(beats[1][aw::field::block_id].as_string(), due->calculate_id().str());
   BOOST_CHECK_LE(beats[1][aw::field::lib_num].as_uint64(), due->block_num());
   BOOST_CHECK_EQUAL(beats[1][aw::field::matches].as_uint64(), 1u);

   // A wall clock stepped back makes one due at once, rather than silencing it until the clock catches up.
   harness.now -= fc::hours(1);
   const signed_block_ptr stepped_back = chain.produce_block();
   BOOST_REQUIRE_EQUAL(harness.of(aw::event::heartbeat).size(), 3u);
   BOOST_CHECK_EQUAL(harness.of(aw::event::heartbeat)[2][aw::field::block_num].as_uint64(), stepped_back->block_num());
   harness.now += fc::seconds(9);
   chain.produce_block();
   BOOST_CHECK_EQUAL(harness.of(aw::event::heartbeat).size(), 3u);

   // Zero disables it.
   watch(config_of({"alice,*::*"}, {}, aw::default_max_data_bytes, 0));
   chain.produce_blocks(2);
   BOOST_CHECK(harness.of(aw::event::heartbeat).empty());
}

BOOST_FIXTURE_TEST_CASE(start_and_stop_records, chain_fixture) {
   watch(config_of({"alice,*::*", "*,sysio::setcode"}, {"alice@active,sysio::reqauth"}, 64, 30,
                   {"carol,sysio.token::transfer"}));
   harness.watch->report_start();
   transfer(alice, bob, "1.0000 TOK");
   chain.produce_block();
   harness.watch->report_stop();

   const std::vector<fc::variant_object> start = harness.of(aw::event::start);
   BOOST_REQUIRE_EQUAL(start.size(), 1u);
   BOOST_CHECK_EQUAL(start[0][aw::field::event].as_string(), "start");
   BOOST_CHECK(start[0][aw::field::include].as<std::vector<std::string>>() ==
               (std::vector<std::string>{"alice@*,*::*", "*@*,sysio::setcode"}));
   BOOST_CHECK(start[0][aw::field::exclude].as<std::vector<std::string>>() ==
               std::vector<std::string>{"alice@active,sysio::reqauth"});
   BOOST_CHECK(start[0][aw::field::receiver].as<std::vector<std::string>>() ==
               std::vector<std::string>{"carol,sysio.token::transfer"});
   BOOST_CHECK_EQUAL(start[0][aw::field::max_data_bytes].as_uint64(), 64u);
   BOOST_CHECK_EQUAL(start[0][aw::field::heartbeat_seconds].as_uint64(), 30u);

   const std::vector<fc::variant_object> stop = harness.of(aw::event::stop);
   BOOST_REQUIRE_EQUAL(stop.size(), 1u);
   BOOST_CHECK_EQUAL(stop[0][aw::field::matches].as_uint64(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(account_watch_fork_tests)

// A fork switch applies the winning branch's blocks: an action in both branches is reported once per block it is in,
// with the same trx_id and action_ordinal, and an action only in the losing branch is not reported again.
BOOST_FIXTURE_TEST_CASE(fork_switch_reports_each_branch, savanna_cluster::cluster_t) try {
   auto& A = _nodes[0];
   auto& C = _nodes[2];
   auto& D = _nodes[3];
   A.create_accounts({alice});
   A.produce_blocks(3);
   BOOST_REQUIRE_EQUAL(D.head().id(), A.head().id());

   // Both reference a block every node holds, and differ only by expiration.
   const auto reqauth = [&](uint32_t expiration) {
      signed_transaction trx;
      trx.actions.emplace_back(
         std::vector<permission_level>{
            {alice, config::active_name}
      },
         config::system_account_name, "reqauth"_n, fc::raw::pack(alice));
      A.set_transaction_headers(trx, expiration);
      trx.sign(tester::get_private_key(alice, "active"), A.control->get_chain_id());
      return trx;
   };
   signed_transaction both = reqauth(1000);
   signed_transaction only_a = reqauth(1001);

   watch_harness harness;
   harness.attach(*A.control, config_of({"alice,*::*"}));

   set_partition({&C, &D}); // nodes {0, 1} and nodes {2, 3}
   A.produce_block();
   D.produce_block();

   A.push_transaction(only_a);
   A.push_transaction(both);
   const signed_block_ptr a_block = A.produce_block();

   D.produce_block(_block_interval_us * 10); // the later timestamp wins, as neither side has a quorum
   D.push_transaction(both);
   const signed_block_ptr fork_tip = D.produce_block();

   const std::vector<fc::variant_object> before = harness.of(aw::event::match);
   BOOST_REQUIRE_EQUAL(before.size(), 2u);
   BOOST_CHECK_EQUAL(before[0][aw::field::trx_id].as_string(), only_a.id().str());
   BOOST_CHECK_EQUAL(before[1][aw::field::trx_id].as_string(), both.id().str());
   BOOST_CHECK_EQUAL(before[1][aw::field::block_id].as_string(), a_block->calculate_id().str());

   push_blocks(3, 0, fork_tip->block_num() - 2); // the blocks D produced after the split
   BOOST_REQUIRE_EQUAL(A.head().id(), D.head().id());

   const std::vector<fc::variant_object> after = harness.of(aw::event::match);
   BOOST_REQUIRE_EQUAL(after.size(), 3u);
   const fc::variant_object& again = after[2];
   BOOST_CHECK_EQUAL(again[aw::field::trx_id].as_string(), both.id().str());
   BOOST_CHECK_EQUAL(again[aw::field::action_ordinal].as_uint64(), before[1][aw::field::action_ordinal].as_uint64());
   BOOST_CHECK_EQUAL(again[aw::field::block_id].as_string(), fork_tip->calculate_id().str());
   BOOST_CHECK_NE(again[aw::field::block_id].as_string(), before[1][aw::field::block_id].as_string());
}
FC_LOG_AND_RETHROW()

// In irreversible read mode a block is accepted only once irreversible, so its record follows finality and its lib_num
// is at least its own block number.
BOOST_FIXTURE_TEST_CASE(irreversible_mode_reports_once_irreversible, savanna_cluster::cluster_t) try {
   auto& A = _nodes[0];
   A.create_accounts({alice});
   A.produce_block();

   tester irreversible(setup_policy::none, db_read_mode::IRREVERSIBLE);
   watch_harness harness; // declared after the tester, so it disconnects first
   harness.attach(*irreversible.control, config_of({"alice,*::*"}, {}, aw::default_max_data_bytes, 1));

   signed_transaction trx;
   trx.actions.emplace_back(
      std::vector<permission_level>{
         {alice, config::active_name}
   },
      config::system_account_name, "reqauth"_n, fc::raw::pack(alice));
   A.set_transaction_headers(trx);
   trx.sign(tester::get_private_key(alice, "active"), A.control->get_chain_id());
   A.push_transaction(trx);
   const signed_block_ptr block = A.produce_block();

   // Not yet irreversible: the irreversible node has not applied the block.
   A.push_blocks_to(irreversible, block->block_num());
   BOOST_REQUIRE_LT(irreversible.head().block_num(), block->block_num());
   BOOST_CHECK(harness.of(aw::event::match).empty());

   A.produce_blocks(4);
   A.push_blocks_to(irreversible, A.head().block_num());
   BOOST_REQUIRE_GE(irreversible.head().block_num(), block->block_num());
   const std::vector<fc::variant_object> found = harness.of(aw::event::match);
   BOOST_REQUIRE_EQUAL(found.size(), 1u);
   BOOST_CHECK_EQUAL(found[0][aw::field::trx_id].as_string(), trx.id().str());
   BOOST_CHECK_EQUAL(found[0][aw::field::block_num].as_uint64(), block->block_num());
   BOOST_CHECK_GE(found[0][aw::field::lib_num].as_uint64(), block->block_num());
   for (const fc::variant_object& beat : harness.of(aw::event::heartbeat))
      BOOST_CHECK_GE(beat[aw::field::lib_num].as_uint64(), beat[aw::field::block_num].as_uint64());
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
