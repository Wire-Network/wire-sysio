#include <boost/accumulators/numeric/functional_fwd.hpp>
#include <boost/optional/optional.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/tuples.hpp>

#include <fc/exception/exception.hpp>

#define BOOST_ASSERTIONS_PASSED_EQUAL(passed_count)                                                \
   {                                                                                               \
      auto& test_unit = boost::unit_test::framework::current_test_unit();                          \
      auto& results   = boost::unit_test::results_collector_t::instance().results(test_unit.p_id); \
      BOOST_CHECK_EQUAL(results.p_assertions_passed, passed_count);                                \
   }


using namespace fc;
using namespace std::literals;

// Helper template for std::visit with multiple lambdas
template <class... Ts>
struct overloaded : Ts... {
   using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

BOOST_AUTO_TEST_SUITE(traits)

BOOST_AUTO_TEST_CASE(lut_variant_visitor) try {
   enum type_enum : uint8_t { t1, t2 };

   constexpr auto type_enum_mapping = std::tuple{
      std::pair{t1, fc::type_tag<std::string>{}},
      std::pair{t2, fc::type_tag<std::size_t>{}},
   };

   using type_value_v = tuple_pairs_to_variant_t<decltype(type_enum_mapping)>;

   std::string  str{"test_string"};
   std::size_t  size{42};
   type_value_v val1{str};
   type_value_v val2{size};

   auto test_type_fn = [&](type_value_v val) {
      std::visit(overloaded{[&](const std::string& s) { BOOST_CHECK_EQUAL(s, str); },
                            [&](const std::size_t& n) { BOOST_CHECK_EQUAL(n, size); },
                            [&](const double& n) { BOOST_CHECK_EQUAL(n, size); }},
                 val);
   };

   test_type_fn(val1);
   test_type_fn(val2);

   auto& test_unit = boost::unit_test::framework::current_test_unit();
   auto& results   = boost::unit_test::results_collector_t::instance().results(test_unit.p_id);
   BOOST_CHECK_EQUAL(results.p_assertions_passed, 2);
   BOOST_ASSERTIONS_PASSED_EQUAL(3);
}
FC_LOG_AND_RETHROW();

namespace type_enum_test {
enum type_enum : uint8_t { type_1, type_2 };
template <type_enum Type>
struct type_base_t {
   type_enum type = Type;
};

struct type_1_t : type_base_t<type_1> {
   std::string str;
};
struct type_2_t : type_base_t<type_2> {
   std::size_t size;
};

static constexpr auto type_enum_mapping = std::tuple{
   std::pair{type_1, fc::type_tag<type_1_t>{}},
   std::pair{type_2, fc::type_tag<type_2_t>{}},
};

using type_enum_mapping_t = std::decay_t<decltype(type_enum_mapping)>;

template <type_enum Type>
using mapped_t = decltype(find_mapped_type<type_enum_mapping, type_enum, Type>());

template <type_enum Type>
constexpr auto make_type_enum_value() {
   return mapped_t<Type>();
}


} // namespace type_enum_test
BOOST_AUTO_TEST_CASE(lut_create_type) try {
   using namespace type_enum_test;
   BOOST_STATIC_ASSERT(std::is_same_v<type_1_t, decltype(make_type_enum_value<type_1>())>);
   auto val1 = make_type_enum_value<type_1>();
   val1.str  = "123";

   auto val2 = make_type_enum_value<type_2>();
   val2.size = 42;
}
FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_SUITE_END()