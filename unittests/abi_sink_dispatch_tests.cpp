#include <boost/test/unit_test.hpp>

// Include order in this file is LOAD-BEARING; do not sort these together.
//
// `abi_sinks.hpp` must come FIRST so that both serializers exercised below are declared
// strictly after the header that defines `stream_sink::emit<T>`.  That is the arrangement
// the emit dispatch has to keep working: "a reflected member type whose JSON serializer is
// declared by a header included later than abi_sinks.hpp".  If that dispatch is ever
// re-qualified as `fc::to_json_stream(...)`, its overload set freezes where abi_sinks.hpp is
// parsed, both serializers below become invisible, their types fall through to the reflector
// primary -- which neither satisfies, having no FC_REFLECT -- and this file fails to compile.
#include <sysio/chain/abi_sinks.hpp>

// Declares fc::to_json_stream / fc::to_variant for the global-namespace softfloat typedefs,
// at the very bottom of the header and therefore well after abi_sinks.hpp.
#include <sysio/chain/database_utils.hpp>

#include <fc/io/json_stream.hpp>
#include <fc/variant.hpp>

#include <cstring>
#include <string>

/// A late-declared serializer in a namespace of its own, reached by ADL on the value.
/// Complements the softfloat case below, which is reached by ADL on the *writer* instead.
namespace late_serializer_ns {
   struct late_thing {
      std::string tag;
   };

   inline void to_json_stream(const late_thing& t, fc::json_writer& w) {
      w.value_string(t.tag);
   }

   inline void to_variant(const late_thing& t, fc::variant& v) {
      v = fc::variant(t.tag);
   }
} // namespace late_serializer_ns

BOOST_AUTO_TEST_SUITE(abi_sink_dispatch_tests)

/// `softfloat128_t` is a global-namespace typedef of an anonymous struct, so ADL on the value
/// associates nothing that can find its serializer; the `fc::json_writer&` parameter is what
/// pulls namespace `fc` into the lookup set at the point of instantiation.  This pins that
/// property against a return to qualified dispatch.
BOOST_AUTO_TEST_CASE(stream_sink_emit_late_declared_fc_serializer) {
   softfloat128_t f;
   memset(&f, 0, sizeof(f));
   f.v[0] = 1;

   std::string out;
   {
      fc::json_writer w(out);
      sysio::chain::impl::stream_sink sink(w);
      sink.emit(f);
   }

   // fc's canonical softfloat128 spelling: "0x" + 16 little-endian hex bytes.
   BOOST_CHECK_EQUAL(out, "\"0x01000000000000000000000000000000\"");
}

/// The other half of the contract: a late-declared serializer that ADL finds through the
/// value's own namespace rather than through the writer.
BOOST_AUTO_TEST_CASE(stream_sink_emit_late_declared_foreign_serializer) {
   std::string out;
   {
      fc::json_writer w(out);
      sysio::chain::impl::stream_sink sink(w);
      sink.emit(late_serializer_ns::late_thing{"wire"});
   }

   BOOST_CHECK_EQUAL(out, "\"wire\"");
}

/// The same type through the variant sink, so the two sinks stay pinned to the same shape
/// and a future change to one cannot silently diverge from the other.
BOOST_AUTO_TEST_CASE(variant_sink_emit_late_declared_foreign_serializer) {
   sysio::chain::impl::variant_sink sink;
   sink.emit(late_serializer_ns::late_thing{"wire"});

   const fc::variant result = std::move(sink).take_result();
   BOOST_CHECK_EQUAL(result.as_string(), "wire");
}

BOOST_AUTO_TEST_SUITE_END()
