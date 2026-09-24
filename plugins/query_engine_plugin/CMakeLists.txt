# Generated code and runtime must describe the same ANTLR release.
find_package(antlr4-runtime CONFIG REQUIRED)
set(QUERY_ANTLR_VERSION "4.13.2")
if(NOT ANTLR_VERSION VERSION_EQUAL QUERY_ANTLR_VERSION)
   message(FATAL_ERROR "query_engine_plugin requires ANTLR ${QUERY_ANTLR_VERSION}; found ${ANTLR_VERSION}")
endif()
if(TARGET antlr4_static)
   set(QUERY_ANTLR_TARGET antlr4_static)
elseif(TARGET antlr4_shared)
   set(QUERY_ANTLR_TARGET antlr4_shared)
else()
   message(FATAL_ERROR "ANTLR runtime exports neither antlr4_static nor antlr4_shared")
endif()

# The parser is generated into the build tree by the vcpkg `antlr4-generator` module whenever the
# grammar changes; nothing generated is committed. The generator is the checksum-pinned JAR the
# `antlr4-tools` port installs, and vcpkg populates CMAKE_PROGRAM_PATH from that host tools tree,
# including during cross-compilation. Generation needs a Java runtime: the module locates it with
# find_package(Java COMPONENTS Runtime REQUIRED).
find_file(ANTLR4_JAR_LOCATION NAMES "antlr-${QUERY_ANTLR_VERSION}-complete.jar"
   PATHS ${CMAKE_PROGRAM_PATH} NO_DEFAULT_PATH REQUIRED)
# The module's default output directory is keyed on ANTLR4_CPP_GENERATED_SRC_DIR but consumed as
# ANTLR4_GENERATED_SRC_DIR; setting both keeps the parser under this plugin's binary directory.
set(ANTLR4_CPP_GENERATED_SRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ANTLR4_GENERATED_SRC_DIR "${ANTLR4_CPP_GENERATED_SRC_DIR}")
find_package(antlr4-generator CONFIG REQUIRED)
antlr4_generate(WireQuery "${CMAKE_CURRENT_SOURCE_DIR}/grammar/WireQuery.g4" BOTH FALSE TRUE "sysio::query_engine")

plugin_target(query_engine_plugin
   LIBRARIES appbase chain_plugin http_plugin producer_plugin ${QUERY_ANTLR_TARGET}
   SOURCE_FILES ${ANTLR4_SRC_FILES_WireQuery})
target_include_directories(query_engine_plugin PRIVATE ${ANTLR4_INCLUDE_DIR_WireQuery} ${ANTLR4_INCLUDE_DIR})

# Runtime-facing contract and examples accompany the node.
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/wire/query_engine_plugin COMPONENT base)
install(DIRECTORY schema examples DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/wire/query_engine_plugin COMPONENT base)
