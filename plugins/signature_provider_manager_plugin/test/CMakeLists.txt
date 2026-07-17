# Tests for the manager plugin itself: the KEY:/KIOD: built-ins, the spec
# grammar, the retain-and-claim machinery, and the startup-probe pass (driven
# with a mock handler -- the AWS-backed schemes' own tests live with their
# plugins under plugins/signature_provider_{ssm,kms}_plugin/test/). main.cpp
# is the Boost.Test entry TU: BOOST_TEST_MODULE must be defined in exactly
# one TU per binary.

set(TARGET_NAME test_signature_provider_manager_plugin)
add_executable(
        ${TARGET_NAME}
        main.cpp
        test_create_provider_specs.cpp
        test_extension_schemes.cpp
)
target_link_libraries(${TARGET_NAME} PRIVATE signature_provider_manager_plugin sysio_testing sysio_chain_wrap)
add_test(NAME ${TARGET_NAME} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
