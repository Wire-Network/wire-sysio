set(TARGET_NAME sysio.depot)

## TODO: Once fully implemented, enable `bootstrap_contract` with static assets
# bootstrap_contract(${TARGET_NAME})
if(BUILD_SYSTEM_CONTRACTS)
    file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp include/*.hpp)
    add_contract(
            ${TARGET_NAME}
            ${TARGET_NAME}
            ${SRC_FILES}
    )

    target_include_directories(
            ${TARGET_NAME}
            PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(
            ${TARGET_NAME}
            PUBLIC
            fc-lite
    )
endif()
