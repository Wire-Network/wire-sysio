
set(TARGET_NAME cranker)

file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp)

chain_target(${TARGET_NAME} SOURCE_FILES ${SRC_FILES})

target_link_libraries(${TARGET_NAME} PRIVATE beacon_chain_update_plugin)
