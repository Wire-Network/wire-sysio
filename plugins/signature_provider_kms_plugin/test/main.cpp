#define BOOST_TEST_MODULE signature_provider_kms_plugin
#include <boost/test/included/unit_test.hpp>

// Boost.Test entry point for the `test_signature_provider_kms_plugin` binary. It bundles the KMS provider unit suite
// (`test_kms_signature_provider.cpp`), the plugin routing suite (`test_kms_plugin_routing.cpp`), and the plugin
// lifecycle suite (`test_kms_plugin_lifecycle.cpp`), all of which contribute `BOOST_AUTO_TEST_SUITE` blocks.
// BOOST_TEST_MODULE must be defined in exactly one TU per binary.
