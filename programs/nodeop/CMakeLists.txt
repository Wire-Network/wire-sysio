
configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

chain_target(${NODE_EXECUTABLE_NAME} SOURCE_FILES main.cpp)

# nodeop opts into the `SSM:` signature-provider scheme (registration in
# main.cpp). The sub-library carries the AWS SDK dependency, deliberately kept
# out of `chain_target`'s shared link set so other chain-tool binaries
# (sys-util, cranker-example) stay AWS-free unless they opt in themselves.
target_link_libraries(${NODE_EXECUTABLE_NAME} PRIVATE sigprov_ssm)

apply_malloc_config(${NODE_EXECUTABLE_NAME})

macro(sysio_additional_plugin)
    set(ADDITIONAL_PLUGINS_TARGET "${ADDITIONAL_PLUGINS_TARGET};${ARGN}" PARENT_SCOPE)
endmacro()

foreach(ADDITIONAL_PLUGIN_SOURCE_DIR ${SYSIO_ADDITIONAL_PLUGINS})
    string(UUID ADDITIONAL_PLUGIN_SOURCE_DIR_MD5 NAMESPACE "00000000-0000-0000-0000-000000000000" NAME ${ADDITIONAL_PLUGIN_SOURCE_DIR} TYPE MD5)
    message(STATUS "[Additional Plugin] ${ADDITIONAL_PLUGIN_SOURCE_DIR} => ${CMAKE_BINARY_DIR}/additional_plugins/${ADDITIONAL_PLUGIN_SOURCE_DIR_MD5}")
    add_subdirectory(${ADDITIONAL_PLUGIN_SOURCE_DIR} ${CMAKE_BINARY_DIR}/additional_plugins/${ADDITIONAL_PLUGIN_SOURCE_DIR_MD5})
endforeach()

foreach(ADDITIONAL_PLUGIN_TARGET ${ADDITIONAL_PLUGINS_TARGET})
    target_link_libraries( ${NODE_EXECUTABLE_NAME}} PRIVATE -Wl,${whole_archive_flag} ${ADDITIONAL_PLUGIN_TARGET} -Wl,${no_whole_archive_flag} )
endforeach()
