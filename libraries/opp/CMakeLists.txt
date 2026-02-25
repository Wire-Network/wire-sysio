# opp: library
set(TARGET_NAME opp)
set(TARGET_MODELS_NAME "${TARGET_NAME}_models")

file(GLOB_RECURSE PROTO_FILES proto/*.proto)

add_library(${TARGET_MODELS_NAME} STATIC)

target_link_libraries(
        ${TARGET_MODELS_NAME}
        PUBLIC
        protobuf::libprotobuf
        fc
        fc-lite
        magic_enum::magic_enum
)

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

protobuf_generate(
        TARGET ${TARGET_MODELS_NAME}
        PROTOS ${PROTO_FILES}
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
        PROTOC_OUT_DIR ${PROTO_GEN_DIR}
)

file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp)
file(GLOB_RECURSE HEADER_FILES include/*.hpp)

# Public include directory for consumers
# The headers are under libraries/opp/include/sysio
target_include_directories(
        ${TARGET_MODELS_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${PROTO_GEN_DIR}>
        $<INSTALL_INTERFACE:include>
)

add_library(
        ${TARGET_NAME}
        STATIC
        ${SRC_FILES}
        ${HEADER_FILES}
)

# Public include directory for consumers
# The headers are under libraries/opp/include/sysio
target_include_directories(
        ${TARGET_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(
        ${TARGET_NAME}
        PUBLIC
        fc
        fc-lite
        magic_enum::magic_enum
        ${TARGET_MODELS_NAME}
)

if(ENABLE_TESTS)
        file(GLOB_RECURSE TEST_FILES test/*.cpp test/*.hpp)
        set(TEST_TARGET_NAME test_opp)
        add_executable(
                ${TEST_TARGET_NAME}
                ${TEST_FILES}
        )
        target_link_libraries(
                ${TEST_TARGET_NAME}
                PRIVATE
                ${TARGET_NAME}
                sysio_testing
                sysio_chain_wrap
        )
        add_test(
                NAME ${TEST_TARGET_NAME}
                COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TEST_TARGET_NAME}
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
endif()

# Installation: headers to include/sysio
install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}
)

install(
        DIRECTORY ${PROTO_GEN_DIR}/
        DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.pb.h"
)

# Install the INTERFACE target for completeness (no artifacts produced)
install(
        TARGETS ${TARGET_NAME}
        EXPORT ${TARGET_NAME}-targets
)
