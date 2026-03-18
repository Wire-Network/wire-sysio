#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/net_utils.hpp>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

using bytes_type = boost::asio::ip::address_v6::bytes_type;

BOOST_AUTO_TEST_SUITE(subnet_mask_tests)

BOOST_AUTO_TEST_CASE(ipv4_mapped_slash24) {
   // ::ffff:192.168.1.100 with /24 → effective /120
   auto addr = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("192.168.1.100"));
   auto masked = sysio::net_utils::apply_prefix_mask(addr.to_bytes(), 96 + 24);

   auto expected = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("192.168.1.0"));
   BOOST_CHECK(masked == expected.to_bytes());
}

BOOST_AUTO_TEST_CASE(ipv4_mapped_same_subnet) {
   // Two addresses in the same /24 should mask to the same value
   auto addr1 = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("10.0.5.17"));
   auto addr2 = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("10.0.5.200"));

   auto masked1 = sysio::net_utils::apply_prefix_mask(addr1.to_bytes(), 96 + 24);
   auto masked2 = sysio::net_utils::apply_prefix_mask(addr2.to_bytes(), 96 + 24);
   BOOST_CHECK(masked1 == masked2);
}

BOOST_AUTO_TEST_CASE(ipv4_mapped_different_subnet) {
   auto addr1 = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("10.0.5.17"));
   auto addr2 = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("10.0.6.17"));

   auto masked1 = sysio::net_utils::apply_prefix_mask(addr1.to_bytes(), 96 + 24);
   auto masked2 = sysio::net_utils::apply_prefix_mask(addr2.to_bytes(), 96 + 24);
   BOOST_CHECK(masked1 != masked2);
}

BOOST_AUTO_TEST_CASE(ipv6_native_slash48) {
   auto addr = boost::asio::ip::make_address_v6("2001:db8:85a3:1234:5678:abcd:ef01:2345");
   auto masked = sysio::net_utils::apply_prefix_mask(addr.to_bytes(), 48);

   auto expected = boost::asio::ip::make_address_v6("2001:db8:85a3::");
   BOOST_CHECK(masked == expected.to_bytes());
}

BOOST_AUTO_TEST_CASE(ipv6_same_slash48_subnet) {
   auto addr1 = boost::asio::ip::make_address_v6("2001:db8:85a3:1111::");
   auto addr2 = boost::asio::ip::make_address_v6("2001:db8:85a3:9999:ffff::");

   auto masked1 = sysio::net_utils::apply_prefix_mask(addr1.to_bytes(), 48);
   auto masked2 = sysio::net_utils::apply_prefix_mask(addr2.to_bytes(), 48);
   BOOST_CHECK(masked1 == masked2);
}

BOOST_AUTO_TEST_CASE(ipv6_different_slash48_subnet) {
   auto addr1 = boost::asio::ip::make_address_v6("2001:db8:85a3::");
   auto addr2 = boost::asio::ip::make_address_v6("2001:db8:85a4::");

   auto masked1 = sysio::net_utils::apply_prefix_mask(addr1.to_bytes(), 48);
   auto masked2 = sysio::net_utils::apply_prefix_mask(addr2.to_bytes(), 48);
   BOOST_CHECK(masked1 != masked2);
}

BOOST_AUTO_TEST_CASE(prefix_0_masks_everything) {
   auto addr = boost::asio::ip::make_address_v6("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
   auto masked = sysio::net_utils::apply_prefix_mask(addr.to_bytes(), 0);

   bytes_type zeroed{};
   BOOST_CHECK(masked == zeroed);
}

BOOST_AUTO_TEST_CASE(prefix_128_preserves_all) {
   auto addr = boost::asio::ip::make_address_v6("2001:db8:85a3:1234:5678:abcd:ef01:2345");
   auto masked = sysio::net_utils::apply_prefix_mask(addr.to_bytes(), 128);
   BOOST_CHECK(masked == addr.to_bytes());
}

BOOST_AUTO_TEST_CASE(prefix_above_128_clamps) {
   auto addr = boost::asio::ip::make_address_v6("2001:db8:85a3:1234:5678:abcd:ef01:2345");
   auto masked = sysio::net_utils::apply_prefix_mask(addr.to_bytes(), 200);
   BOOST_CHECK(masked == addr.to_bytes());
}

BOOST_AUTO_TEST_CASE(ipv4_slash16) {
   auto addr1 = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("172.16.5.100"));
   auto addr2 = boost::asio::ip::make_address_v6(
      boost::asio::ip::v4_mapped,
      boost::asio::ip::make_address_v4("172.16.200.1"));

   auto masked1 = sysio::net_utils::apply_prefix_mask(addr1.to_bytes(), 96 + 16);
   auto masked2 = sysio::net_utils::apply_prefix_mask(addr2.to_bytes(), 96 + 16);
   BOOST_CHECK(masked1 == masked2);
}

BOOST_AUTO_TEST_SUITE_END()
