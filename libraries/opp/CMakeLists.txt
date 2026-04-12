# opp: library
set(TARGET_NAME opp)
set(TARGET_MODELS_NAME "${TARGET_NAME}_models")
set(TARGET_CDT_MODELS_NAME "${TARGET_NAME}_cdt_models")

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PROTO_CDT_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated-cdt)

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

if (TARGET cdt::protoc-gen-zpp)
    find_cdt_magic_enum()

    # Build the list of expected .pb.hpp outputs from each .proto file
    set(_ZPP_OUTPUT_HDRS)
    foreach(_proto ${PROTO_FILES})
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${_proto}")
        string(REGEX REPLACE "\\.proto$" ".pb.hpp" _rel_hpp "${_rel}")
        list(APPEND _ZPP_OUTPUT_HDRS "${PROTO_CDT_GEN_DIR}/${_rel_hpp}")
    endforeach()

    file(MAKE_DIRECTORY ${PROTO_CDT_GEN_DIR})

    add_custom_command(
            COMMENT "Generating zpp protobuf headers from proto files"
            OUTPUT ${_ZPP_OUTPUT_HDRS}
            COMMAND cdt::protoc
              -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
              --plugin=protoc-gen-zpp=$<TARGET_FILE:cdt::protoc-gen-zpp>
              --zpp_out ${PROTO_CDT_GEN_DIR}
              ${PROTO_FILES}
            DEPENDS cdt::protoc-gen-zpp ${PROTO_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}

    )
    add_custom_target(${TARGET_CDT_MODELS_NAME}.protos ALL
            DEPENDS ${_ZPP_OUTPUT_HDRS}
    )
    add_library(${TARGET_CDT_MODELS_NAME} INTERFACE)
    add_dependencies(${TARGET_CDT_MODELS_NAME} ${TARGET_CDT_MODELS_NAME}.protos)

    target_include_directories(${TARGET_CDT_MODELS_NAME}
            INTERFACE
            $<BUILD_INTERFACE:${PROTO_CDT_GEN_DIR}>
            $<INSTALL_INTERFACE:include>
    )

    if(BUILD_SYSTEM_CONTRACTS AND TARGET contracts_project)
        message(NOTICE "BUILD_SYSTEM_CONTRACTS is enabled AND contracts_project is a target, attaching dependency on ${TARGET_CDT_MODELS_NAME} to contracts_project")
        add_dependencies(contracts_project ${TARGET_CDT_MODELS_NAME})
    else()
        message(NOTICE "Either BUILD_SYSTEM_CONTRACTS is NOT enabled OR contracts_project is NOT a target, not attaching dependency on ${TARGET_CDT_MODELS_NAME} to contracts_project")
    endif()
endif()

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
