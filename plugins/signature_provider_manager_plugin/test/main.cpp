#define BOOST_TEST_MODULE signature_provider_manager_plugin
#include <boost/test/included/unit_test.hpp>

// Boost.Test entry point for the `test_signature_provider_manager_plugin`
// binary (the plugin's own, AWS-free tests). The kms/ssm binaries built from
// this directory have their own entry TUs (`kms_main.cpp` / `ssm_main.cpp`):
// BOOST_TEST_MODULE must be defined in exactly one TU per binary.

// Note: Boost.Test runs test cases sequentially by default.
// To ensure sequential execution, run without --run_test or parallel flags.
