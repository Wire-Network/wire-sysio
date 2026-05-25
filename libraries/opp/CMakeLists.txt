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

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

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

if(BUILD_OPP_BUNDLES)
    message(STATUS "BUILD_OPP_BUNDLES is enabled, checking for required programs to generate OPP bundles (TypeScript, Solidity, Solana) after protobuf generation.")

    # After protobuf generation, regenerate OPP bundles (TS/Solidity/Solana models)
    set(_required_progs fish npm pnpm node)
    set(_missing_progs)
    foreach(_prog ${_required_progs})
        # Use a distinct cache variable per tool: find_program short-circuits when
        # its output variable is already set in the cache, so reusing one variable
        # across iterations would return the first tool's path for every subsequent
        # lookup and silently mark all tools as "found".
        string(MAKE_C_IDENTIFIER "${_prog}" _prog_var)
        find_program(_found_prog_${_prog_var} ${_prog})
        if(NOT _found_prog_${_prog_var})
            list(APPEND _missing_progs ${_prog})
        endif()
    endforeach()

    set(_version_check_failed FALSE)
    set(_version_errors)

    if(NOT _missing_progs)
        # Check node version >= 24.0.0
        execute_process(
                COMMAND node --version
                OUTPUT_VARIABLE _node_version_output
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _node_version_result
        )
        if(_node_version_result EQUAL 0)
            string(REGEX REPLACE "^v([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\1" _node_major "${_node_version_output}")
            if(_node_major LESS 24)
                set(_version_check_failed TRUE)
                list(APPEND _version_errors "node version ${_node_version_output} is too old (required >= 24.0.0)")
            endif()
        else()
            set(_version_check_failed TRUE)
            list(APPEND _version_errors "failed to check node version")
        endif()

        # Check pnpm version >= 10.0.0
        execute_process(
                COMMAND pnpm --version
                OUTPUT_VARIABLE _pnpm_version_output
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _pnpm_version_result
        )
        if(_pnpm_version_result EQUAL 0)
            string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\1" _pnpm_major "${_pnpm_version_output}")
            if(_pnpm_major LESS 10)
                set(_version_check_failed TRUE)
                list(APPEND _version_errors "pnpm version ${_pnpm_version_output} is too old (required >= 10.0.0)")
            endif()
        else()
            set(_version_check_failed TRUE)
            list(APPEND _version_errors "failed to check pnpm version")
        endif()
    endif()

    if(_missing_progs OR _version_check_failed)
        set(_error_msg "BUILD_OPP_BUNDLES is ON, but requirements are not met:")
        if(_missing_progs)
            string(APPEND _error_msg " missing programs: ${_missing_progs};")
        endif()
        if(_version_errors)
            string(APPEND _error_msg " ${_version_errors};")
        endif()
        string(APPEND _error_msg " OPP bundles (TypeScript, Solidity, Solana) will NOT be generated.")
        message(WARNING "${_error_msg}")
    else()
        message(STATUS "BUILD_OPP_BUNDLES: All required programs found: ${_required_progs}. OPP bundles (TypeScript, Solidity, Solana) will be generated after protobuf generation.")
        add_custom_command(
                TARGET ${TARGET_NAME}
                POST_BUILD
                COMMENT "Generating OPP bundles (TypeScript, Solidity, Solana)"
                COMMAND fish ${CMAKE_CURRENT_SOURCE_DIR}/tools/scripts/generate-opp-bundles.fish
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools
        )
    endif()
endif()
if(NOT TARGET cdt::protoc-gen-zpp)
    if(BUILD_SYSTEM_CONTRACTS)
        message(FATAL_ERROR "cdt::protoc-gen-zpp target not found, CDT protobuf support is required.")
    else()
        message(STATUS "cdt::protoc-gen-zpp target not found, but BUILD_SYSTEM_CONTRACTS is disabled, so skipping CDT protobuf generation as expected.")
    endif()
endif()

if(TARGET cdt::protoc-gen-zpp)
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
        add_dependencies(contracts_project ${TARGET_CDT_MODELS_NAME} ${TARGET_CDT_MODELS_NAME}.protos)
        if(TARGET contracts_project-build)
            add_dependencies(contracts_project-build ${TARGET_CDT_MODELS_NAME}.protos)
        endif()
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
