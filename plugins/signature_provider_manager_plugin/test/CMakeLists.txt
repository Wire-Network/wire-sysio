# Three test binaries build from this directory, one per provider family,
# because their link lines differ deliberately: the plugin's own tests stay
# AWS-free (matching the plugin target), while the kms/ssm tests link the
# AWS-backed sub-libraries they exercise. Sources are listed explicitly --
# a glob cannot express the per-binary split of a flat directory. Each binary
# has its own Boost.Test entry TU (main.cpp / kms_main.cpp / ssm_main.cpp):
# BOOST_TEST_MODULE must be defined in exactly one TU per binary, so a shared
# entry point cannot carry it.

set(TARGET_NAME test_signature_provider_manager_plugin)
add_executable(
        ${TARGET_NAME}
        main.cpp
        test_create_provider_specs.cpp
)
target_link_libraries(${TARGET_NAME} PRIVATE signature_provider_manager_plugin sysio_testing sysio_chain_wrap Boost::process Boost::dll)
add_test(NAME ${TARGET_NAME} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

set(KMS_TARGET_NAME test_sigprov_kms)

add_executable(${KMS_TARGET_NAME}
        kms_main.cpp
        test_kms_plugin_routing.cpp
        test_kms_signature_provider.cpp)

target_link_libraries(${KMS_TARGET_NAME}
        PRIVATE
        sigprov_kms
        # The plugin-routing test stands up an appbase application and calls
        # plugin methods directly, so it needs the full plugin (which pulls
        # in appbase) -- the headers-only shim that sigprov_kms links is not
        # enough for the test TU.
        signature_provider_manager_plugin
        sysio_testing
        sysio_chain_wrap
        fc-test
        # NOTE: do not link Boost::unit_test_framework here. The test entry
        # point in kms_main.cpp uses <boost/test/included/unit_test.hpp>
        # (header-only Boost.Test), matching the plugin's own test binary.
        # Linking the prebuilt library as well would pull in a second main()
        # and a second `init_unit_test_suite` symbol.
        Boost::process
        Boost::dll)

add_test(NAME ${KMS_TARGET_NAME}
         COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${KMS_TARGET_NAME}
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

set(SSM_TARGET_NAME test_sigprov_ssm)

add_executable(${SSM_TARGET_NAME}
        ssm_main.cpp
        test_ssm_plugin_routing.cpp
        test_ssm_signature_provider.cpp)

target_link_libraries(${SSM_TARGET_NAME}
        PRIVATE
        sigprov_ssm
        # The plugin-routing test stands up an appbase application and calls
        # plugin methods directly, so it needs the full plugin (which pulls
        # in appbase) -- the headers-only shim that sigprov_ssm links is not
        # enough for the test TU.
        signature_provider_manager_plugin
        sysio_testing
        sysio_chain_wrap
        fc-test)
        # NOTE: do not link Boost::unit_test_framework here. The test entry
        # point in ssm_main.cpp uses <boost/test/included/unit_test.hpp>
        # (header-only Boost.Test), matching the plugin's own test binary.
        # Linking the prebuilt library as well would pull in a second main()
        # and a second `init_unit_test_suite` symbol. Unlike the kms sibling,
        # no Boost::process / Boost::dll here: these tests only load keygen
        # fixtures from disk, never spawn the keygen scripts.

add_test(NAME ${SSM_TARGET_NAME}
         COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${SSM_TARGET_NAME}
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
