#include <fc/exception/exception.hpp>
#include <fc/network/url.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>

BOOST_AUTO_TEST_SUITE(url_suite)

BOOST_AUTO_TEST_CASE(port_holds_its_whole_range) {
   BOOST_CHECK(!fc::url("http://example.com/path").port());
   BOOST_CHECK_EQUAL(*fc::url("http://example.com:0/path").port(), 0u);
   BOOST_CHECK_EQUAL(*fc::url("http://example.com:8888/path").port(), 8888u);
   BOOST_CHECK_EQUAL(*fc::url("http://example.com:65535").port(), std::numeric_limits<uint16_t>::max());
   BOOST_CHECK_THROW(fc::url("http://example.com:65536"), fc::parse_error_exception);
}

// A negative port used to parse as its two's complement, so this one read as port 255.
BOOST_AUTO_TEST_CASE(port_rejects_a_negative_number) {
   BOOST_CHECK_THROW(fc::url("http://example.com:-1/path"), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::url("http://example.com:-18446744073709551361/path"), fc::parse_error_exception);
}

BOOST_AUTO_TEST_SUITE_END()
