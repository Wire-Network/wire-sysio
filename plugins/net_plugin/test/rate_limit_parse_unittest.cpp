#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/net_utils.hpp>

BOOST_AUTO_TEST_CASE(test_parse_rate_limit) {
   std::vector<std::string> p2p_addresses = {
        "0.0.0.0:9876"
      , "0.0.0.0:9776:0"
      , "0.0.0.0:9877:640KB/s"
      , "192.168.0.1:9878:20MiB/s"
      , "localhost:9879:0.5KB/s"
      , "[2001:db8:85a3:8d3:1319:8a2e:370:7348]:9876:250KB/s"
      , "[::1]:9876:250KB/s"
      , "2001:db8:85a3:8d3:1319:8a2e:370:7348:9876:250KB/s"
      , "[::1]:9876:-250KB/s"
      , "0.0.0.0:9877:640Kb/s"
      , "0.0.0.0:9877:999999999999999999999999999TiB/s"
      , "0.0.0.0:9876:trx"
      , "0.0.0.0:9776:blk:0"
      , "0.0.0.0:9877:trx:640KB/s"
      , "192.168.0.1:9878:blk:20MiB/s"
      , "localhost:9879:trx:0.5KB/s"
      , "[2001:db8:85a3:8d3:1319:8a2e:370:7348]:9876:trx:250KB/s"
      , "[::1]:9876:trx:250KB/s"
      , "2001:db8:85a3:8d3:1319:8a2e:370:7348:9876:trx:250KB/s"
      , "[::1]:9876:trx:-1KB/s"
      , "0.0.0.0:9877:trx:640Kb/s"
      , "0.0.0.0:9877:trx:999999999999999999999999999TiB/s"
      , "0.0.0.0:9875 - 84c470d"
      , "0.0.0.0:9876:trx - 84c470d"
      , "0.0.0.0:9877:trx:640KB/s - additional info"
      , "[2001:db8:85a3:8d3:1319:8a2e:370:7348]additional info:trx:640KB/s"
      , "0.0.0.0"
      , "0.0.0.0:"
      , "0.0.0.0::"
      , "0.0.0.0:9877:640"
      , "0.0.0.0:9877:trx:640"
      , "0.0.0.0:9877:640B/s"
   };
   size_t which = 0;
   auto [listen_addr, block_sync_rate_limit] = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 0u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9776");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 0u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9877");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 640000u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "192.168.0.1:9878");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 20971520u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "localhost:9879");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 500u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "2001:db8:85a3:8d3:1319:8a2e:370:7348:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 250000u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "::1:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 250000u);
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "IPv6 addresses must be enclosed in square brackets");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "block sync rate limit must not be negative");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "invalid block sync rate limit specification");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "block sync rate limit specification overflowed");});
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 0u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9776");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 0u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9877");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 640000u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "192.168.0.1:9878");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 20971520u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "localhost:9879");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 500u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "2001:db8:85a3:8d3:1319:8a2e:370:7348:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 250000u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "::1:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 250000u);
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "IPv6 addresses must be enclosed in square brackets");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "block sync rate limit must not be negative");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "invalid block sync rate limit specification");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "block sync rate limit specification overflowed");});
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9875");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 0u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9876");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 0u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9877");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 640000u);
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "unexpected character after ']'");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "unexpected number of colons");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "host or port missing");});
   BOOST_CHECK_EXCEPTION(sysio::net_utils::parse_listen_address(p2p_addresses.at(which++)), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e)
                         {return std::strstr(e.top_message().c_str(), "host or port missing");});
   // A rate cap without a unit is bytes per second, the same as `B/s`.
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9877");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 640u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9877");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 640u);
   std::tie(listen_addr, block_sync_rate_limit) = sysio::net_utils::parse_listen_address(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(listen_addr, "0.0.0.0:9877");
   BOOST_CHECK_EQUAL(block_sync_rate_limit, 640u);
}

BOOST_AUTO_TEST_CASE(test_split_host_port_type) {
   std::vector<std::string> p2p_addresses = {
        "0.0.0.0:9876"
      , "0.0.0.0:9776:0"
      , "0.0.0.0:9877:640KB/s"
      , "192.168.0.1:9878:20MiB/s"
      , "localhost:9879:0.5KB/s"
      , "[2001:db8:85a3:8d3:1319:8a2e:370:7348]:9876:250KB/s"
      , "[::1]:9876:250KB/s"
      , "2001:db8:85a3:8d3:1319:8a2e:370:7348:9876:250KB/s"
      , "[::1]:9876:-250KB/s"
      , "0.0.0.0:9877:640Kb/s"
      , "0.0.0.0:9877:999999999999999999999999999TiB/s"
      , "0.0.0.0:9876:trx"
      , "0.0.0.0:9776:blk:0"
      , "0.0.0.0:9877:trx:640KB/s"
      , "192.168.0.1:9878:blk:20MiB/s"
      , " localhost:9879:trx:0.5KB/s"
      , "[2001:db8:85a3:8d3:1319:8a2e:370:7348]:9876:trx:250KB/s"
      , "[::1]:9876:trx:250KB/s"
      , "2001:db8:85a3:8d3:1319:8a2e:370:7348:9876:trx:250KB/s"
      , "[::1]:9876:trx:-1KB/s"
      , "0.0.0.0:9877:trx:640Kb/s"
      , "0.0.0.0:9877:trx:999999999999999999999999999TiB/s"
      , "0.0.0.0:9876 - 84c470d"
      , "0.0.0.0:9876:trx - 84c470d"
      , "0.0.0.0:9877:trx:640KB/s - additional info"
      , "[2001:db8:85a3:8d3:1319:8a2e:370:7348]additional info:trx:640KB/s"
      , "0.0.0.0"
      , "0.0.0.0:"
      , "0.0.0.0::"
   };
   size_t which = 0;
   auto [host, port, type] = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9776");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "192.168.0.1");
   BOOST_CHECK_EQUAL(port, "9878");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "localhost");
   BOOST_CHECK_EQUAL(port, "9879");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "2001:db8:85a3:8d3:1319:8a2e:370:7348");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "::1");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "");
   BOOST_CHECK_EQUAL(port, "");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "::1");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9776");
   BOOST_CHECK_EQUAL(type, "blk");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "192.168.0.1");
   BOOST_CHECK_EQUAL(port, "9878");
   BOOST_CHECK_EQUAL(type, "blk");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "localhost");
   BOOST_CHECK_EQUAL(port, "9879");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "2001:db8:85a3:8d3:1319:8a2e:370:7348");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "::1");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "");
   BOOST_CHECK_EQUAL(port, "");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "::1");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9877");
   BOOST_CHECK_EQUAL(type, "trx");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "");
   BOOST_CHECK_EQUAL(port, "");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "");
   BOOST_CHECK_EQUAL(port, "");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "");
   BOOST_CHECK_EQUAL(port, "");
   BOOST_CHECK_EQUAL(type, "");
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type(p2p_addresses.at(which++));
   BOOST_CHECK_EQUAL(host, "");
   BOOST_CHECK_EQUAL(port, "");
   BOOST_CHECK_EQUAL(type, "");
}

BOOST_AUTO_TEST_CASE(test_is_unspecified_host) {
   BOOST_CHECK(sysio::net_utils::is_unspecified_host(""));
   BOOST_CHECK(sysio::net_utils::is_unspecified_host("0.0.0.0"));
   BOOST_CHECK(sysio::net_utils::is_unspecified_host("::"));
   BOOST_CHECK(sysio::net_utils::is_unspecified_host("0:0:0:0:0:0:0:0"));
   BOOST_CHECK(!sysio::net_utils::is_unspecified_host("127.0.0.1"));
   BOOST_CHECK(!sysio::net_utils::is_unspecified_host("::1"));
   BOOST_CHECK(!sysio::net_utils::is_unspecified_host("localhost"));
   BOOST_CHECK(!sysio::net_utils::is_unspecified_host("192.168.0.1"));

   // A bracketed IPv6 listen address reaches the check with its brackets already stripped.
   const auto [host, port, type] = sysio::net_utils::split_host_port_type("[::]:9876:trx");
   BOOST_CHECK_EQUAL(host, "::");
   BOOST_CHECK(sysio::net_utils::is_unspecified_host(host));
}

// A single-colon address carries no `:trx|:blk` or `:<rate>` section, so everything after the port digits is trailing
// prose (net_plugin appends ` - <peer id>` when it logs an address) and must not be read as either.
BOOST_AUTO_TEST_CASE(test_single_colon_trailing_text) {
   auto [host, port, type] = sysio::net_utils::split_host_port_type("0.0.0.0:9876 - 84c470d");
   BOOST_CHECK_EQUAL(host, "0.0.0.0");
   BOOST_CHECK_EQUAL(port, "9876");
   BOOST_CHECK_EQUAL(type, "");

   auto [addr, rate] = sysio::net_utils::parse_listen_address("0.0.0.0:9876 - 84c470d");
   BOOST_CHECK_EQUAL(addr, "0.0.0.0:9876");
   BOOST_CHECK_EQUAL(rate, 0u);

   // A type glued to the port without its colon is not the documented syntax.
   std::tie(host, port, type) = sysio::net_utils::split_host_port_type("0.0.0.0:9876trx");
   BOOST_CHECK_EQUAL(type, "");
}

// A port is a decimal number from 0 to 65535. The resolver would wrap a larger one into that range and read a name as
// a service, so both are invalid addresses.
BOOST_AUTO_TEST_CASE(test_port_number) {
   using sysio::net_utils::parse_listen_address;
   using sysio::net_utils::split_host_port_type;

   for (const std::string valid : {"0", "1", "9876", "65535", "09876", "000065535"}) {
      BOOST_TEST_CONTEXT(valid) {
         for (const std::string suffix : {"", " - 84c470d", ":trx", ":blk:640KB/s"}) {
            const auto [host, port, type] = split_host_port_type("0.0.0.0:" + valid + suffix);
            BOOST_CHECK_EQUAL(host, "0.0.0.0");
            BOOST_CHECK_EQUAL(port, valid);
         }
         BOOST_CHECK_EQUAL(std::get<0>(parse_listen_address("[::1]:" + valid + ":640KB/s")), "::1:" + valid);
      }
   }

   // Out of range, with and without the sections that may follow a port.
   for (const std::string invalid : {"65536", "70000", "99999", "4294967296", "18446744073709551616"}) {
      BOOST_TEST_CONTEXT(invalid) {
         for (const std::string suffix : {"", " - 84c470d", ":trx", ":blk:640KB/s"}) {
            for (const std::string host_text : {"0.0.0.0", "localhost", "[::1]"}) {
               const std::string address = host_text + ":" + invalid + suffix;
               const auto [host, port, type] = split_host_port_type(address);
               BOOST_CHECK_EQUAL(host, "");
               BOOST_CHECK_EQUAL(port, "");
               BOOST_CHECK_EQUAL(type, "");
               BOOST_CHECK_EXCEPTION(parse_listen_address(address), sysio::chain::plugin_config_exception,
                                     [](const sysio::chain::plugin_config_exception& e) {
                                        return e.top_message().find("port must be a number no greater than 65535") !=
                                               std::string::npos;
                                     });
            }
         }
      }
   }

   // Not a number. Only a port followed by another section can carry these: after a lone colon, text that follows the
   // port digits is trailing prose.
   for (const std::string invalid : {"http", "98x6", "x9876", "9876x", "-1", "+80", " 80", "80 ", "0x50", "8.0"}) {
      BOOST_TEST_CONTEXT(invalid) {
         for (const std::string suffix : {":trx", ":blk:640KB/s", ":640KB/s"}) {
            const std::string address = "0.0.0.0:" + invalid + suffix;
            const auto [host, port, type] = split_host_port_type(address);
            BOOST_CHECK_EQUAL(host, "");
            BOOST_CHECK_EQUAL(port, "");
            BOOST_CHECK_EQUAL(type, "");
            BOOST_CHECK_EXCEPTION(parse_listen_address(address), sysio::chain::plugin_config_exception,
                                  [](const sysio::chain::plugin_config_exception& e) {
                                     return e.top_message().find("port must be a number") != std::string::npos;
                                  });
         }
      }
   }

   // An empty port keeps its own diagnostic.
   BOOST_CHECK_EXCEPTION(parse_listen_address("0.0.0.0::trx"), sysio::chain::plugin_config_exception,
                         [](const sysio::chain::plugin_config_exception& e) {
                            return e.top_message().find("host or port missing") != std::string::npos;
                         });
}
