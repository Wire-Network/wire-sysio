# Shared AWS-SDK glue for the signature-provider sub-libraries (kms/, ssm/),
# built as its own static library so both link one owner of the SDK lifecycle.
# Aws::InitAPI / Aws::ShutdownAPI must run exactly once per process; if each
# sub-library carried its own lifecycle singleton, a binary linking both would
# double-init / double-shutdown the SDK. This library also carries the shared
# per-region client cache, the transient-vs-permanent error split, and the
# service-agnostic ARN parsing pieces.
set(TARGET_NAME sigprov_aws_common)

find_package(aws-cpp-sdk-core CONFIG REQUIRED)

file(GLOB_RECURSE ${TARGET_NAME}_headers
        include/*.hpp include/*.h src/*.hpp src/*.h)

add_library(${TARGET_NAME}
        src/aws_common.cpp
        ${${TARGET_NAME}_headers})

target_include_directories(${TARGET_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME}
        PUBLIC
        # `fc` for the exception machinery named in the public header;
        # `sysio_chain` for the chain exception types the error split throws.
        fc
        sysio_chain
        aws-cpp-sdk-core)

# Internal plugin support library: linked into nodeop, never shipped as a
# standalone artifact (see cmake/plugin-tools.cmake for rationale).

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)
