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

# vcpkg populates CMAKE_PROGRAM_PATH from the host tools tree, including during cross-compilation.
find_file(QUERY_ANTLR_JAR NAMES "antlr-${QUERY_ANTLR_VERSION}-complete.jar"
   PATHS ${CMAKE_PROGRAM_PATH} NO_DEFAULT_PATH REQUIRED)
find_package(Java COMPONENTS Runtime QUIET)
if(Java_FOUND)
   set(QUERY_ANTLR_JAVA "${Java_JAVA_EXECUTABLE}")
else()
   set(QUERY_ANTLR_JAVA java)
endif()
set(QUERY_GENERATE_PARSER FALSE)
foreach(component Lexer Parser Visitor BaseVisitor)
   foreach(extension h cpp)
      set(parser_file "${CMAKE_CURRENT_SOURCE_DIR}/generated/WireQuery${component}.${extension}")
      if(NOT EXISTS "${parser_file}")
         set(QUERY_GENERATE_PARSER TRUE)
      else()
         file(SIZE "${parser_file}" parser_size)
         if(parser_size EQUAL 0)
            set(QUERY_GENERATE_PARSER TRUE)
         endif()
      endif()
   endforeach()
endforeach()
if(QUERY_GENERATE_PARSER)
   if(NOT Java_FOUND)
      message(FATAL_ERROR "Parser generation requires a system JVM. On Ubuntu 24.04 install openjdk-21-jre-headless from the default Ubuntu repositories.")
   endif()
   message(STATUS "Generating query parser with vcpkg tool ${QUERY_ANTLR_JAR}")
   execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "ANTLR_JAR=${QUERY_ANTLR_JAR}" "ANTLR_JAVA=${QUERY_ANTLR_JAVA}"
         "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-parser.sh" generate
      RESULT_VARIABLE QUERY_GENERATE_RESULT)
   if(NOT QUERY_GENERATE_RESULT STREQUAL "0")
      message(FATAL_ERROR "Query parser generation failed: ${QUERY_GENERATE_RESULT}")
   endif()
endif()
plugin_target(query_engine_plugin
   LIBRARIES appbase chain_plugin http_plugin ${QUERY_ANTLR_TARGET}
   SOURCE_FILES
      generated/WireQueryLexer.cpp
      generated/WireQueryParser.cpp
      generated/WireQueryVisitor.cpp
      generated/WireQueryBaseVisitor.cpp)
target_include_directories(query_engine_plugin PRIVATE generated ${ANTLR4_INCLUDE_DIR})

# Runtime-facing contract and examples accompany the node; parser generation remains developer-only.
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/wire/query_engine_plugin COMPONENT base)
install(DIRECTORY schema examples DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/wire/query_engine_plugin COMPONENT base)
add_custom_target(check_query_parser
   COMMAND "${CMAKE_COMMAND}" -E env "ANTLR_JAR=${QUERY_ANTLR_JAR}" "ANTLR_JAVA=${QUERY_ANTLR_JAVA}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-parser.sh" check
   COMMENT "Checking the ANTLR parser with the vcpkg-installed generator"
   VERBATIM)
