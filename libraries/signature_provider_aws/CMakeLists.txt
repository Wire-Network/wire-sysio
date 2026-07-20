# Shared AWS-SDK glue for the AWS-backed signature-provider plugins
# (signature_provider_kms_plugin, signature_provider_ssm_plugin), built as one
# static library so every provider links ONE owner of the SDK lifecycle:
# Aws::InitAPI / Aws::ShutdownAPI must run exactly once per process; if each
# provider carried its own lifecycle singleton, a binary linking more than one
# would double-init / double-shutdown the SDK. This library also carries the
# shared per-region client cache, the transient-vs-permanent error split, and
# the service-agnostic ARN parsing pieces.
find_package(aws-cpp-sdk-core CONFIG REQUIRED)

add_library(signature_provider_aws
        src/aws_common.cpp
        include/sysio/signature_provider_aws/aws_common.hpp)

target_include_directories(signature_provider_aws PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(signature_provider_aws
        PUBLIC
        # `fc` for the exception machinery named in the public header;
        # `sysio_chain` for the chain exception types the error split throws.
        fc
        sysio_chain
        aws-cpp-sdk-core)

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)

if (ENABLE_TESTS)
    add_subdirectory(test)
endif ()
