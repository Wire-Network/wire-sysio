#include <boost/test/unit_test.hpp>
#include <fc/exception/exception.hpp>
#include <fc/static_variant.hpp>
#include <fc/variant.hpp>

// Include order below static_variant.hpp is LOAD-BEARING, do not sort these up.
// fc::sha256 gets its JSON serializer from the FC_SERIALIZE_AS_STRING trait declared in
// fc/serialize_as_string.hpp, which static_variant.hpp does not pull in.  Including it here --
// after static_variant.hpp -- is exactly the "alternative declared later" arrangement that
// to_json_stream(std::variant) has to keep working: if that template ever dispatches its
// alternatives with a qualified fc::to_json_stream again, the overload set is fixed where the
// template is defined, fc::sha256 falls through to the reflector primary (it is only
// FC_REFLECT_TYPENAME'd, never FC_REFLECT'd) and this translation unit fails to compile.
#include <fc/crypto/sha256.hpp>
#include <fc/io/json_stream.hpp>

BOOST_AUTO_TEST_SUITE(static_variant_test_suite)
   BOOST_AUTO_TEST_CASE(to_from_fc_variant)
   {
      using variant_type = std::variant<int32_t, bool>;
      auto std_variant_1 = variant_type{false};
      auto fc_variant = fc::variant{};

      fc::to_variant(std_variant_1, fc_variant);

      auto std_variant_2 = variant_type{};
      fc::from_variant(fc_variant, std_variant_2);

      BOOST_REQUIRE(std_variant_1 == std_variant_2);
   }

   BOOST_AUTO_TEST_CASE(get)
   {
     using variant_type = std::variant<int32_t, bool, std::string>;

     auto v1 = variant_type{std::string{"hello world"}};
     BOOST_CHECK_EXCEPTION(std::get<int32_t>(v1), std::bad_variant_access, [](const auto& e) { return true; });
     auto result1 = std::get<std::string>(v1);
     BOOST_REQUIRE(result1 == std::string{"hello world"});

     const auto v2 = variant_type{std::string{"hello world"}};
     BOOST_CHECK_EXCEPTION(std::get<int32_t>(v2), std::bad_variant_access, [](const auto& e) { return true; });
     const auto result2 = std::get<std::string>(v2);
     BOOST_REQUIRE(result2 == std::string{"hello world"});
   }

   BOOST_AUTO_TEST_CASE(static_variant_from_index)
   {
      using variant_type = std::variant<int32_t, bool, std::string>;
      auto v = variant_type{};

      BOOST_CHECK_EXCEPTION(fc::from_index(v, 3), fc::assert_exception, [](const auto& e) { return e.code() == fc::assert_exception_code; });

      fc::from_index(v, 2);
      BOOST_REQUIRE(std::string{} == std::get<std::string>(v));
   }

   /// to_json_stream(std::variant) must dispatch its alternatives through ADL, so an alternative
   /// whose serializer is declared after static_variant.hpp still resolves.  fc::sha256 is that
   /// alternative here (see the include-order note at the top of this file): with a qualified
   /// fc::to_json_stream in the visitor, the overload set is frozen where the template is defined,
   /// sha256 falls through to the reflector primary and this test does not compile.
   BOOST_AUTO_TEST_CASE(to_json_stream_late_declared_alternative)
   {
      using variant_type = std::variant<fc::sha256, int32_t>;

      const auto hash = fc::sha256::hash(std::string{"wire"});
      const auto hashed = variant_type{hash};

      BOOST_CHECK_EQUAL(fc::to_json_string(hashed), "[0,\"" + hash.str() + "\"]");
      BOOST_CHECK_EQUAL(fc::to_json_string(variant_type{int32_t{7}}), "[1,7]");

      // ... and the streamed shape stays byte-identical to the variant path's.
      BOOST_CHECK_EQUAL(fc::to_json_string(hashed),
                        fc::json::to_string(fc::variant(hashed), fc::json::yield_function_t()));
   }

   BOOST_AUTO_TEST_CASE(static_variant_get_index)
   {
      using variant_type = std::variant<int32_t, bool, std::string>;
      BOOST_REQUIRE((fc::get_index<variant_type, int32_t>() == 0));
      BOOST_REQUIRE((fc::get_index<variant_type, bool>() == 1));
      BOOST_REQUIRE((fc::get_index<variant_type, std::string>() == 2));
      BOOST_REQUIRE((fc::get_index<variant_type, double>() == std::variant_size_v<variant_type>)); // Isn't a type contained in variant.
   }
BOOST_AUTO_TEST_SUITE_END()
