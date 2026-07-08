#define BOOST_TEST_MODULE sigprov_kms
#include <boost/test/included/unit_test.hpp>

// Boost.Test entry point for the `test_sigprov_kms` binary. It bundles the
// KMS-library unit suite (`test_kms_signature_provider.cpp`) and the plugin
// routing suite (`test_kms_plugin_routing.cpp`), both of which contribute
// `BOOST_AUTO_TEST_SUITE` blocks.
