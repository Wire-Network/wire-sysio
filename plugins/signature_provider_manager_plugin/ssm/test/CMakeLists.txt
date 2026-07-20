file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS *.cpp)
set(TARGET_NAME test_sigprov_ssm)

add_executable(${TARGET_NAME} ${SRC_FILES})

target_link_libraries(${TARGET_NAME}
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
        # point in main.cpp uses <boost/test/included/unit_test.hpp> (header-
        # only Boost.Test), matching the plugin's own test binary. Linking
        # the prebuilt library as well would pull in a second main() and a
        # second `init_unit_test_suite` symbol. Unlike the kms sibling, no
        # Boost::process / Boost::dll here: these tests only load keygen
        # fixtures from disk, never spawn the keygen scripts.

add_test(NAME ${TARGET_NAME}
         COMMAND ${CMAKE_BINARY_DIR}/plugins/signature_provider_manager_plugin/ssm/test/${TARGET_NAME}
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
