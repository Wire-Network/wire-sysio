# Offline unit binary for the shared AWS glue, plus the `signature_provider_aws_test` INTERFACE target
# carrying the RAII environment fixtures (env var overrides, shared-config redirect, the
# nothing-resolves bundle) that the kms/ssm plugin suites share -- region resolution reads live process
# state, and every suite must pin and restore it the same way. main.cpp is the Boost.Test entry TU using
# header-only Boost.Test (<boost/test/included/unit_test.hpp>): do not link Boost::unit_test_framework
# here -- the prebuilt library would pull in a second main().

add_library(signature_provider_aws_test INTERFACE)
target_include_directories(signature_provider_aws_test INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(signature_provider_aws_test INTERFACE signature_provider_aws)

set(TARGET_NAME test_signature_provider_aws)

add_executable(${TARGET_NAME}
        main.cpp
        test_aws_common.cpp)

target_link_libraries(${TARGET_NAME}
        PRIVATE
        signature_provider_aws_test)

add_test(NAME ${TARGET_NAME}
         COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
