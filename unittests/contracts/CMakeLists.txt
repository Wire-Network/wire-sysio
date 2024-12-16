# will be implictly used for any compilation unit if not overrided by SYSTEM_INCLUDE_FOLDERS parameter
# these directories go as -isystem <dir> to avoid warnings from code of third-party libraries

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/sysio.bios/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sysio.bios/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/sysio.msig/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sysio.msig/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/sysio.roa/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sysio.roa/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/sysio.system/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sysio.system/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/sysio.token/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sysio.token/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/sysio.wrap/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sysio.wrap/)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/old_versions/v1.6.0-rc3/sysio.bios/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/old_versions/v1.6.0-rc3/sysio.bios/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/old_versions/v1.7.0-develop-preactivate_feature/sysio.bios/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/old_versions/v1.7.0-develop-preactivate_feature/sysio.bios/)
