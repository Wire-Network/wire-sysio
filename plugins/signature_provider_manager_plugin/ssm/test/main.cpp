#define BOOST_TEST_MODULE sigprov_ssm
#include <boost/test/included/unit_test.hpp>

// Boost.Test entry point for the `test_sigprov_ssm` binary. It bundles the
// SSM-library unit suite (`test_ssm_signature_provider.cpp`) and the plugin
// routing suite (`test_ssm_plugin_routing.cpp`), both of which contribute
// `BOOST_AUTO_TEST_SUITE` blocks.
