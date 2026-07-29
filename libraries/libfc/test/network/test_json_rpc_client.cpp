/**
 * @file test_json_rpc_client.cpp
 * @brief Regression tests for deadline-bound JSON-RPC transport calls.
 */

#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>
#include <fc-test/scripted_json_rpc_server.hpp>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr size_t OVERSIZED_RESPONSE_BODY_BYTES = 2 * 1024 * 1024;
static_assert(std::is_move_constructible_v<fc::network::json_rpc::json_rpc_client>);
static_assert(std::is_move_assignable_v<fc::network::json_rpc::json_rpc_client>);

/** Return a JSON-RPC error object suitable for a scripted response. */
fc::variant json_rpc_error_object(int64_t code, std::string message) {
   return fc::variant(fc::mutable_variant_object()("code", code)("message", std::move(message)));
}

/**
 * Return true when the exception came from the transport response body limit.
 */
bool is_response_body_limit_error(const fc::exception& e) {
   return e.to_detail_string().find("response_limit") != std::string::npos;
}

} // namespace

BOOST_AUTO_TEST_SUITE(json_rpc_client_tests)

/// Legacy JSON-RPC clients retain startup DNS until a connection failure invalidates it.
BOOST_AUTO_TEST_CASE(default_endpoint_refresh_policy_is_preserved) {
   const fc::network::json_rpc::client_options options;

   BOOST_CHECK(!options.transport.dns_cache_timeout);
   BOOST_CHECK(
      options.transport.refresh_dns_on_connection_failure);
}

/// One healthy persistent connection is validated exactly once before use.
BOOST_AUTO_TEST_CASE(connection_validator_runs_once_per_persistent_connection) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("cluster-a"),
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   std::atomic_uint32_t attempts{0};
   std::atomic_uint32_t successes{0};
   fc::network::json_rpc::client_options options;
   options.connection_validator = fc::network::json_rpc::connection_validation{
      .method = "wire_chain_identity",
      .validate_result =
         [](const fc::variant& result) {
            FC_ASSERT(result.as_string() == "cluster-a", "Unexpected JSON-RPC connection identity");
         },
      .on_attempt = [&] { attempts.fetch_add(1); },
      .on_success = [&] { successes.fetch_add(1); },
   };
   fc::network::json_rpc::json_rpc_client client(
      fc::url(server.url()),
      std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
      std::move(options));

   BOOST_CHECK_EQUAL(client.call_idempotent("wire_first").as_string(), "first");
   BOOST_CHECK_EQUAL(client.call_idempotent("wire_second").as_string(), "second");
   const std::vector<std::string> expected_methods{"wire_chain_identity", "wire_first", "wire_second"};
   BOOST_CHECK(server.request_methods() == expected_methods);
   BOOST_CHECK_EQUAL(server.connection_count(), 1u);
   BOOST_CHECK_EQUAL(attempts.load(), 1u);
   BOOST_CHECK_EQUAL(successes.load(), 1u);
}

/// Replacement connections cannot inherit a previous socket's validation.
BOOST_AUTO_TEST_CASE(connection_validator_runs_again_after_reconnect) {
   auto first = fc::test::scripted_json_rpc_response::result("first");
   first.reset_after_response = true;
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("cluster-a"),
      std::move(first),
      fc::test::scripted_json_rpc_response::result("cluster-a"),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   std::atomic_uint32_t attempts{0};
   fc::network::json_rpc::client_options options;
   options.connection_validator = fc::network::json_rpc::connection_validation{
      .method = "wire_chain_identity",
      .validate_result = [](const fc::variant& result) {
         FC_ASSERT(result.as_string() == "cluster-a", "Unexpected JSON-RPC connection identity");
      },
      .on_attempt = [&] { attempts.fetch_add(1); },
   };
   fc::network::json_rpc::json_rpc_client client(
      fc::url(server.url()),
      std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
      std::move(options));

   BOOST_CHECK_EQUAL(client.call_idempotent("wire_first").as_string(), "first");
   BOOST_CHECK_EQUAL(client.call_idempotent("wire_second").as_string(), "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 2u);
   BOOST_CHECK_EQUAL(attempts.load(), 2u);
}

/// Validation cannot bless a socket that the peer closes before ordinary work.
BOOST_AUTO_TEST_CASE(connection_validator_rejects_nonpersistent_response) {
   auto validation = fc::test::scripted_json_rpc_response::result("cluster-a");
   validation.keep_alive = false;
   fc::test::scripted_json_rpc_server server({std::move(validation)});
   fc::network::json_rpc::client_options options;
   options.connection_validator = fc::network::json_rpc::connection_validation{
      .method = "wire_chain_identity",
      .validate_result = [](const fc::variant&) {},
   };
   fc::network::json_rpc::json_rpc_client client(
      fc::url(server.url()),
      std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
      std::move(options));

   BOOST_CHECK_EXCEPTION(
      client.call_idempotent("wire_protected"),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("did not preserve a persistent connection") != std::string::npos;
      });
   BOOST_CHECK_EQUAL(server.request_count(), 1u);
}

/// Concurrent callers reserve distinct JSON-RPC IDs before transport admission.
BOOST_AUTO_TEST_CASE(concurrent_calls_use_unique_request_ids) {
   constexpr size_t caller_count = 16;
   std::vector<fc::test::scripted_json_rpc_response> responses;
   responses.reserve(caller_count);
   for (size_t index = 0; index < caller_count; ++index)
      responses.push_back(fc::test::scripted_json_rpc_response::result("ok"));
   fc::test::scripted_json_rpc_server server(std::move(responses));
   auto client = std::make_shared<fc::network::json_rpc::json_rpc_client>(fc::url(server.url()));

   std::vector<std::future<fc::variant>> calls;
   calls.reserve(caller_count);
   for (size_t index = 0; index < caller_count; ++index) {
      calls.push_back(std::async(std::launch::async, [client] { return client->call_idempotent("wire_read"); }));
   }
   for (auto& call : calls)
      BOOST_CHECK_EQUAL(call.get().as_string(), "ok");

   std::set<int64_t> ids;
   for (const auto& request : server.requests()) {
      BOOST_REQUIRE(request.id);
      ids.insert(*request.id);
   }
   BOOST_CHECK_EQUAL(ids.size(), caller_count);
}

/// Moving a used client preserves the monotonic JSON-RPC request-ID sequence.
BOOST_AUTO_TEST_CASE(move_preserves_next_request_id) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EQUAL(client.call("wire_first").as_string(), "first");
   auto moved = std::move(client);
   BOOST_CHECK_EQUAL(moved.call("wire_second").as_string(), "second");

   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2u);
   BOOST_REQUIRE(requests[0].id);
   BOOST_REQUIRE(requests[1].id);
   BOOST_CHECK_EQUAL(*requests[0].id, 1);
   BOOST_CHECK_EQUAL(*requests[1].id, 2);
}

/// Server destruction closes a pooled keep-alive connection held by a longer-lived client.
BOOST_AUTO_TEST_CASE(server_destruction_does_not_wait_for_longer_lived_client) {
   std::optional<fc::network::json_rpc::json_rpc_client> client;
   const auto start = std::chrono::steady_clock::now();
   {
      fc::test::scripted_json_rpc_server server({
         fc::test::scripted_json_rpc_response::result("ok"),
      });
      client.emplace(fc::url(server.url()));
      BOOST_CHECK_EQUAL(client->call("wire_keep_alive").as_string(), "ok");
   }
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(elapsed, std::chrono::seconds(2));
   client.reset();
}

/// The shared fixture consumes JSON-RPC notifications without manufacturing an invalid response.
BOOST_AUTO_TEST_CASE(scripted_server_handles_notification_without_response_id) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("ignored"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_NO_THROW(client.notify("wire_notification"));
   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 1u);
   BOOST_CHECK_EQUAL(requests.front().method, "wire_notification");
   BOOST_CHECK(!requests.front().id);
}

/// Explicitly idempotent calls reuse one healthy connection.
BOOST_AUTO_TEST_CASE(idempotent_calls_reuse_a_healthy_connection) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_first_probe").as_string(),
      "first");
   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_second_probe").as_string(),
      "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 1U);
   BOOST_CHECK_EQUAL(server.request_count(), 2U);
}

/// A result-aware JSON-RPC hook selects a follow-up on the exact connection.
BOOST_AUTO_TEST_CASE(continuation_hook_selects_same_connection_followup) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   bool hook_called = false;
   const auto result = client.call_then("wire_identity_probe", fc::variants{},
                                        fc::network::json_rpc::call_options{
                                           .replay = fc::network::json_rpc::replay_policy::stale_reused_connection_once,
                                           .total_timeout_cap = fc::seconds(1),
                                        },
                                        [&](const fc::variant& first_result) {
                                           hook_called = true;
                                           BOOST_CHECK_EQUAL(first_result.as_string(), "first");
                                           return fc::network::json_rpc::continuation_call{
                                              .method = "wire_protected_operation",
                                              .options =
                                                 fc::network::json_rpc::follow_up_options{
                                                    .total_timeout_cap = fc::seconds(1),
                                                 },
                                           };
                                        });

   BOOST_CHECK(hook_called);
   BOOST_CHECK_EQUAL(result.as_string(), "second");
   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_CHECK_EQUAL(requests[0].connection, requests[1].connection);
}

/// An initial JSON-RPC error rejects the continuation before invoking its hook.
BOOST_AUTO_TEST_CASE(continuation_initial_json_rpc_error_prevents_hook) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::error(json_rpc_error_object(-32000, "probe failed")),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::network::json_rpc::json_rpc_error);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A malformed initial envelope rejects the continuation before invoking its hook.
BOOST_AUTO_TEST_CASE(continuation_malformed_initial_envelope_prevents_hook) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::raw("{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":\"first\"}"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A mismatched initial response ID rejects the continuation before invoking its hook.
BOOST_AUTO_TEST_CASE(continuation_wrong_initial_id_prevents_hook) {
   auto response = fc::test::scripted_json_rpc_response::result("first");
   response.response_id = 99;
   fc::test::scripted_json_rpc_server server({std::move(response)});
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A rejecting hook closes the retained connection without sending a follow-up.
BOOST_AUTO_TEST_CASE(continuation_hook_rejection_prevents_followup) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [](const fc::variant&) -> fc::network::json_rpc::continuation_call {
                                         FC_THROW("identity rejected");
                                      }),
                     fc::exception);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A JSON-RPC error from the follow-up is returned to the caller.
BOOST_AUTO_TEST_CASE(continuation_followup_json_rpc_error_is_reported) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::error(json_rpc_error_object(-32001, "operation failed")),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [](const fc::variant&) {
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::network::json_rpc::json_rpc_error);
   BOOST_CHECK_EQUAL(server.requests().size(), 2U);
}

/// A mismatched follow-up response ID is rejected.
BOOST_AUTO_TEST_CASE(continuation_wrong_followup_id_is_rejected) {
   auto wrong_id = fc::test::scripted_json_rpc_response::result("second");
   wrong_id.response_id = 99;
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
      std::move(wrong_id),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [](const fc::variant&) {
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK_EQUAL(server.requests().size(), 2U);
}

/// A stale cached first connection replays only the probe and retains the replacement for the follow-up.
BOOST_AUTO_TEST_CASE(continuation_replays_stale_initial_connection_once) {
   auto stale = fc::test::scripted_json_rpc_response::result("warm");
   stale.reset_after_response = true;
   fc::test::scripted_json_rpc_server server({
      std::move(stale),
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EQUAL(client.call_idempotent("wire_warm_connection").as_string(), "warm");
   const auto result =
      client.call_then("wire_identity_probe", fc::variants{},
                       fc::network::json_rpc::call_options{
                          .replay = fc::network::json_rpc::replay_policy::stale_reused_connection_once,
                       },
                       [](const fc::variant&) {
                          return fc::network::json_rpc::continuation_call{.method = "wire_protected_operation"};
                       });

   BOOST_CHECK_EQUAL(result.as_string(), "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 2U);
   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 3U);
   BOOST_CHECK_NE(requests[0].connection, requests[1].connection);
   BOOST_CHECK_EQUAL(requests[1].connection, requests[2].connection);
}

/// A non-replaying continuation probe fails instead of replacing a stale cached connection.
BOOST_AUTO_TEST_CASE(continuation_never_policy_does_not_replay_stale_initial_connection) {
   auto stale = fc::test::scripted_json_rpc_response::result("warm");
   stale.reset_after_response = true;
   fc::test::scripted_json_rpc_server server(
      {std::move(stale)},
      "127.0.0.1",
      0,
      true);
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_EQUAL(client.call_idempotent("wire_warm_connection").as_string(), "warm");
   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.connection_count(), 1U);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A total-timeout cap preserves stricter configured phase deadlines.
BOOST_AUTO_TEST_CASE(continuation_timeout_cap_preserves_stricter_phase_timeout) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::delayed_result("first", 250ms),
   });
   fc::network::json_rpc::client_options options;
   options.request.timeouts.header = fc::milliseconds(40);
   options.request.timeouts.total = fc::seconds(2);
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()), std::nullopt,
                                                 fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
                                                 std::move(options));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{},
                                      fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(500)},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::timeout_exception);

   BOOST_CHECK(!hook_called);
}

/// A per-call cap cannot lengthen a stricter base total timeout.
BOOST_AUTO_TEST_CASE(continuation_timeout_cap_cannot_expand_base_total_timeout) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::delayed_result("first", 250ms),
   });
   fc::network::json_rpc::client_options options;
   options.request.timeouts.header = fc::seconds(1);
   options.request.timeouts.total = fc::milliseconds(40);
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()), std::nullopt,
                                                 fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
                                                 std::move(options));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{},
                                      fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(500)},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::timeout_exception);

   BOOST_CHECK(!hook_called);
}

/// Connection validation cannot extend the initiating call's already-started total budget.
BOOST_AUTO_TEST_CASE(connection_validation_inherits_initiating_call_total_deadline) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::delayed_result("cluster-a", 250ms),
   });
   fc::network::json_rpc::client_options options;
   options.request.timeouts.total = fc::seconds(1);
   options.connection_validator = fc::network::json_rpc::connection_validation{
      .method = "wire_chain_identity",
      .total_timeout_cap = fc::seconds(1),
      .validate_result = [](const fc::variant&) {},
   };
   fc::network::json_rpc::json_rpc_client client(
      fc::url(server.url()),
      std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
      std::move(options));
   bool hook_called = false;

   BOOST_CHECK_THROW(
      client.call_then(
         "wire_protected_operation",
         fc::variants{},
         fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(50)},
         [&](const fc::variant&) {
            hook_called = true;
            return fc::network::json_rpc::continuation_call{.method = "wire_follow_up"};
         }),
      fc::timeout_exception);

   BOOST_CHECK(!hook_called);
   BOOST_CHECK(server.request_methods() == std::vector<std::string>{"wire_chain_identity"});
}

/// The follow-up receives a fresh total-timeout budget independent of the probe.
BOOST_AUTO_TEST_CASE(continuation_followup_has_independent_total_timeout) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::result("first"),
      fc::test::scripted_json_rpc_response::delayed_result("second", 150ms),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   const auto result = client.call_then("wire_identity_probe", fc::variants{},
                                        fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(50)},
                                        [](const fc::variant&) {
                                           return fc::network::json_rpc::continuation_call{
                                              .method = "wire_protected_operation",
                                              .options =
                                                 fc::network::json_rpc::follow_up_options{
                                                    .total_timeout_cap = fc::milliseconds(500),
                                                 },
                                           };
                                        });

   BOOST_CHECK_EQUAL(result.as_string(), "second");
}

/// An idempotent call retries once when its cached connection has gone stale.
BOOST_AUTO_TEST_CASE(idempotent_call_recovers_from_a_stale_cached_connection) {
   auto stale = fc::test::scripted_json_rpc_response::result("first");
   stale.reset_after_response = true;
   fc::test::scripted_json_rpc_server server({
      std::move(stale),
      fc::test::scripted_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_first_probe").as_string(),
      "first");
   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_second_probe").as_string(),
      "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 2U);
   BOOST_CHECK_EQUAL(server.request_count(), 2U);
}

/// Caller-supplied retry options cannot make a default call replay.
BOOST_AUTO_TEST_CASE(default_call_enforces_single_attempt) {
   auto warm =
      fc::test::scripted_json_rpc_response::result("warm");
   fc::test::scripted_json_rpc_server server({
      std::move(warm),
      fc::test::scripted_json_rpc_response::close(),
      fc::test::scripted_json_rpc_response::result("replayed"),
   });
   fc::network::json_rpc::client_options options;
   options.request.retry.max_attempts = 3;
   options.request.retry.initial_backoff = fc::microseconds(0);
   options.request.retry.max_backoff = fc::microseconds(0);
   options.request.retry.allow_retry =
      [](const fc::http::retry_context& context) {
         return context.reused_connection;
      };
   options.request.idempotent = true;
   fc::network::json_rpc::json_rpc_client client(
      fc::url(server.url()),
      std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
      std::move(options));

   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_first_probe").as_string(),
      "warm");
   BOOST_CHECK_THROW(
      client.call("wire_side_effect_probe"),
      fc::exception);
   BOOST_CHECK_EQUAL(
      server.connection_count(),
      1U);
   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_CHECK_EQUAL(
      requests.front().method,
      "wire_first_probe");
   BOOST_CHECK_EQUAL(
      requests.back().method,
      "wire_side_effect_probe");
}

/// URL parsing preserves bracketed IPv6 identity, credentials, path, query, and port.
BOOST_AUTO_TEST_CASE(url_round_trips_ipv6_authority_and_query) {
   const fc::url parsed("https://operator:secret@[2001:db8::1]:8443/rpc?commitment=finalized");

   BOOST_REQUIRE(parsed.host());
   BOOST_CHECK_EQUAL(*parsed.host(), "2001:db8::1");
   BOOST_REQUIRE(parsed.port());
   BOOST_CHECK_EQUAL(*parsed.port(), 8443U);
   BOOST_REQUIRE(parsed.path());
   BOOST_CHECK_EQUAL(parsed.path()->generic_string(), "/rpc");
   BOOST_REQUIRE(parsed.query());
   BOOST_CHECK_EQUAL(*parsed.query(), "commitment=finalized");
   BOOST_CHECK_EQUAL(
      static_cast<std::string>(parsed),
      "https://operator:secret@[2001:db8::1]:8443/rpc?commitment=finalized");
}

/// Diagnostic endpoint labels omit URL credentials, paths, and queries.
BOOST_AUTO_TEST_CASE(endpoint_diagnostics_are_credential_free) {
   const fc::url endpoint(
      "https://operator:secret@[2001:db8::1]:8443/"
      "private/token?authorization=hidden");

   const auto sanitized =
      fc::http::sanitized_endpoint(endpoint);
   BOOST_CHECK_EQUAL(
      sanitized,
      "https://[2001:db8::1]:8443");
   BOOST_CHECK(
      sanitized.find("operator") == std::string::npos);
   BOOST_CHECK(
      sanitized.find("secret") == std::string::npos);
   BOOST_CHECK(
      sanitized.find("authorization") == std::string::npos);
}

/// A peer that accepts the TCP request but withholds the HTTP response must
/// release the caller within the active RPC deadline.
BOOST_AUTO_TEST_CASE(call_times_out_when_http_response_hangs) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::hanging(),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   const auto start = fc::time_point::now();
   BOOST_CHECK_THROW(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() + fc::milliseconds(200));
         client.call("wire_deadline_probe");
      }(),
      fc::timeout_exception);
   const auto elapsed = fc::time_point::now() - start;

   BOOST_CHECK_LT(elapsed.count(), 1500 * 1000);
}

/// An already-expired ambient deadline must fail before network I/O starts.
BOOST_AUTO_TEST_CASE(call_rejects_expired_ambient_deadline) {
   fc::network::json_rpc::json_rpc_client client(fc::url("http://localhost:9876"));

   BOOST_CHECK_THROW(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() - fc::milliseconds(1));
         client.call("wire_expired_deadline_probe");
      }(),
      fc::timeout_exception);
}

/// A peer that completes HTTP 200 with an oversized body must be rejected by
/// the transport parser before JSON parsing or outpost envelope decoding.
BOOST_AUTO_TEST_CASE(call_rejects_oversized_response_body) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::raw(std::string(OVERSIZED_RESPONSE_BODY_BYTES, 'x')),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EXCEPTION(
      client.call("wire_body_limit_probe"),
      fc::exception,
      is_response_body_limit_error);
}

/// The raw HTTP helper shares the same bounded transport path as JSON-RPC calls.
BOOST_AUTO_TEST_CASE(send_http_rejects_oversized_response_body) {
   fc::test::scripted_json_rpc_server server({
      fc::test::scripted_json_rpc_response::raw(std::string(OVERSIZED_RESPONSE_BODY_BYTES, 'x')),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EXCEPTION(
      client.send_http(fc::network::json_rpc::http_verb::GET, "/"),
      fc::exception,
      is_response_body_limit_error);
}

BOOST_AUTO_TEST_SUITE_END()
