#include <boost/test/unit_test.hpp>

#include <fc/move_only_function.hpp>

#include <memory>
#include <utility>

// On libstdc++/newer libc++ the fc::move_only_function alias resolves to
// std::move_only_function, so the polyfill branch never compiles there.  These cases
// instantiate fc::detail::move_only_function_impl directly so the polyfill gets
// compile + behavior coverage on every platform, not just AppleClang.
BOOST_AUTO_TEST_SUITE(move_only_function_tests)

BOOST_AUTO_TEST_CASE(polyfill_void_nullary) {
   int calls = 0;
   fc::detail::move_only_function_impl<void()> f{ [&] { ++calls; } };
   BOOST_REQUIRE(static_cast<bool>(f));
   f();
   BOOST_CHECK_EQUAL(calls, 1);

   fc::detail::move_only_function_impl<void()> empty;
   BOOST_CHECK(!static_cast<bool>(empty));
   fc::detail::move_only_function_impl<void()> null_constructed{ nullptr };
   BOOST_CHECK(!static_cast<bool>(null_constructed));
}

BOOST_AUTO_TEST_CASE(polyfill_returns_value_and_forwards_args) {
   fc::detail::move_only_function_impl<int(int, int)> add{ [](int a, int b) { return a + b; } };
   BOOST_CHECK_EQUAL(add(2, 40), 42);

   // Move-only by-value argument must forward through the type-erasure boundary.
   fc::detail::move_only_function_impl<int(std::unique_ptr<int>)> take{
      [](std::unique_ptr<int> p) { return *p; } };
   BOOST_CHECK_EQUAL(take(std::make_unique<int>(7)), 7);

   // Rvalue-reference parameter -- the shape chain::next_function stores.
   fc::detail::move_only_function_impl<void(std::unique_ptr<int>&&)> sink{
      [](std::unique_ptr<int>&& p) { auto consumed = std::move(p); BOOST_CHECK_EQUAL(*consumed, 9); } };
   sink(std::make_unique<int>(9));
}

BOOST_AUTO_TEST_CASE(polyfill_move_only_capture_and_move_semantics) {
   auto payload = std::make_unique<int>(11);
   fc::detail::move_only_function_impl<int()> f{ [p = std::move(payload)] { return *p; } };

   // Move construction and move assignment transfer the callable.
   fc::detail::move_only_function_impl<int()> g{ std::move(f) };
   BOOST_CHECK(!static_cast<bool>(f));
   BOOST_REQUIRE(static_cast<bool>(g));
   BOOST_CHECK_EQUAL(g(), 11);

   fc::detail::move_only_function_impl<int()> h;
   h = std::move(g);
   BOOST_REQUIRE(static_cast<bool>(h));
   BOOST_CHECK_EQUAL(h(), 11);
}

BOOST_AUTO_TEST_CASE(polyfill_nested_shape) {
   // Mirrors url_response_stream_callback: a move-only function taking a move-only
   // function by value.
   using inner_t = fc::detail::move_only_function_impl<void(int&)>;
   using outer_t = fc::detail::move_only_function_impl<void(int, inner_t)>;

   int observed = 0;
   outer_t outer{ [&](int code, inner_t emit) {
      int v = code;
      emit(v);
      observed = v;
   } };
   outer(200, inner_t{ [](int& v) { v += 22; } });
   BOOST_CHECK_EQUAL(observed, 222);
}

BOOST_AUTO_TEST_CASE(alias_basic_usage) {
   // Whichever implementation the alias selects on this platform, the common subset
   // (construct from move-only capture, bool conversion, single invocation) must work.
   auto payload = std::make_unique<int>(5);
   fc::move_only_function<int()> f{ [p = std::move(payload)] { return *p; } };
   BOOST_REQUIRE(static_cast<bool>(f));
   BOOST_CHECK_EQUAL(f(), 5);
}

BOOST_AUTO_TEST_SUITE_END()
