
set(TARGET_NAME cranker-example)

file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp)

chain_target(${TARGET_NAME} SOURCE_FILES ${SRC_FILES})
