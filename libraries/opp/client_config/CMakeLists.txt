set(TARGET_MODELS_NAME opp_client_config_models)
set(TARGET_NAME opp_client_config)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

file(GLOB_RECURSE PROTO_FILES CONFIGURE_DEPENDS proto/*.proto)

add_library(${TARGET_MODELS_NAME} STATIC)
target_link_libraries(${TARGET_MODELS_NAME} PUBLIC protobuf::libprotobuf)

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
protobuf_generate(
        TARGET ${TARGET_MODELS_NAME}
        PROTOS ${PROTO_FILES}
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
        PROTOC_OUT_DIR ${PROTO_GEN_DIR}
)
target_include_directories(
        ${TARGET_MODELS_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${PROTO_GEN_DIR}>
        $<INSTALL_INTERFACE:include>
)

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS include/*.hpp)
add_library(${TARGET_NAME} STATIC ${SRC_FILES} ${HEADER_FILES})
target_include_directories(
        ${TARGET_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(
        ${TARGET_NAME}
        PUBLIC
        ${TARGET_MODELS_NAME}
        fc
)

if(ENABLE_TESTS)
    file(GLOB_RECURSE TEST_FILES CONFIGURE_DEPENDS test/*.cpp)
    set(TEST_TARGET_NAME test_opp_client_config)
    add_executable(${TEST_TARGET_NAME} ${TEST_FILES})
    target_link_libraries(${TEST_TARGET_NAME} PRIVATE ${TARGET_NAME})
    add_test(
            NAME ${TEST_TARGET_NAME}
            COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TEST_TARGET_NAME}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
