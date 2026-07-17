# One binary bundling the SSM provider unit suite, the plugin routing suite,
# and the plugin lifecycle suite. main.cpp is the Boost.Test entry TU using
# header-only Boost.Test (<boost/test/included/unit_test.hpp>): do not link
# Boost::unit_test_framework here -- the prebuilt library would pull in a
# second main() and a second `init_unit_test_suite` symbol. Unlike the kms
# sibling, no Boost::process / Boost::dll: these tests only load keygen
# fixtures from disk, never spawn the keygen scripts.

set(TARGET_NAME test_signature_provider_ssm_plugin)

add_executable(${TARGET_NAME}
        main.cpp
        test_ssm_plugin_lifecycle.cpp
        test_ssm_plugin_routing.cpp
        test_ssm_signature_provider.cpp)

target_link_libraries(${TARGET_NAME}
        PRIVATE
        # The plugin target carries the provider machinery, the plugin class,
        # and (transitively) the manager plugin the routing/lifecycle suites
        # drive directly.
        signature_provider_ssm_plugin
        sysio_testing
        sysio_chain_wrap
        fc-test)

add_test(NAME ${TARGET_NAME}
         COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
