#[=======================================================================[
boost_test_discover_tests(<target> NAME <template> [options...])

Registers one CTest entry per top-level unit (a suite, or a test case declared outside any suite) of a Boost.Test
executable. The units are read from the executable's own `--list_content` output when ctest starts, the way CMake's
gtest_discover_tests(DISCOVERY_MODE PRE_TEST) works for GoogleTest, so adding or removing a suite needs no CMake edit.

  NAME <template>                 CTest name. @UNIT@ is the Boost unit, @NAME@ the unit after NAME_STRIP, and
                                  @RUNTIME@ the runtime when RUNTIMES is given.
  NAME_STRIP <regex>...           Patterns removed from @UNIT@, in order, to form @NAME@.
  RUNTIMES <runtime>...           One entry per unit per runtime, each run with `-- --<runtime>`.
  INCLUDE <regex>                 Register only the units that match.
  EXCLUDE <regex>                 Skip the units that match.
  ARGS <arg>...                   Arguments after `--run_test=<unit>`.
  WORKING_DIRECTORY <dir>         Defaults to CMAKE_BINARY_DIR.
  PROPERTIES <name> <value>...    Single-valued test properties set on every entry.
  COST_BY_NAME <regex> <cost>...  COST for the entries whose @NAME@ matches; the first match wins.

A unit disabled by default is registered DISABLED. Until the executable is built a failing <target>_NOT_BUILT entry
stands in, and if the executable cannot list its units a failing <target>_DISCOVERY_FAILED entry reports why. The list
is cached until the executable or these options change.
#]=======================================================================]

set(_BOOST_TEST_DISCOVERY_IMPL "${CMAKE_CURRENT_LIST_DIR}/boost-test-discovery-impl.cmake")

function(boost_test_discover_tests TARGET)
   cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;INCLUDE;EXCLUDE;WORKING_DIRECTORY"
                         "NAME_STRIP;RUNTIMES;ARGS;PROPERTIES;COST_BY_NAME")
   if(NOT arg_NAME OR arg_UNPARSED_ARGUMENTS)
      message(FATAL_ERROR "boost_test_discover_tests(${TARGET}) needs NAME; unexpected: ${arg_UNPARSED_ARGUMENTS}")
   endif()
   # Lists reach the ctest-time implementation joined into single arguments, so no value may contain `;`.
   foreach(option IN ITEMS NAME_STRIP RUNTIMES ARGS PROPERTIES COST_BY_NAME)
      foreach(value IN LISTS arg_${option})
         if(value MATCHES ";")
            message(FATAL_ERROR "boost_test_discover_tests(${TARGET}): ${option} values cannot contain `;`: ${value}")
         endif()
      endforeach()
   endforeach()
   foreach(pairs IN ITEMS PROPERTIES COST_BY_NAME)
      list(LENGTH arg_${pairs} length)
      math(EXPR odd "${length} % 2")
      if(odd)
         message(FATAL_ERROR "boost_test_discover_tests(${TARGET}): ${pairs} takes pairs, got: ${arg_${pairs}}")
      endif()
   endforeach()
   if(NOT arg_WORKING_DIRECTORY)
      set(arg_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
   endif()

   # Each call gets its own files and placeholder names, so one executable can be discovered with different options.
   get_property(call TARGET ${TARGET} PROPERTY BOOST_TEST_DISCOVERY_CALLS)
   if(NOT call)
      set(call 0)
   endif()
   math(EXPR call "${call} + 1")
   set_property(TARGET ${TARGET} PROPERTY BOOST_TEST_DISCOVERY_CALLS ${call})
   set(placeholder ${TARGET})
   if(call GREATER 1)
      set(placeholder ${TARGET}_${call})
   endif()
   set(file_base "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_discovery_${call}")
   get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
   set(tests_file "${file_base}_tests.cmake")
   if(multi_config)
      set(tests_file "${file_base}_tests-$<CONFIG>.cmake")
   endif()

   set(placeholder_properties "")
   foreach(value IN LISTS arg_PROPERTIES)
      string(APPEND placeholder_properties " [==[${value}]==]")
   endforeach()

   # Runs when ctest reads this directory's tests; the implementation rediscovers only when the list is stale.
   string(CONCAT content
      "if(EXISTS [==[$<TARGET_FILE:${TARGET}>]==])\n"
      "   include([==[${_BOOST_TEST_DISCOVERY_IMPL}]==])\n"
      "   boost_test_discover_tests_impl(\n"
      "      EXECUTABLE [==[$<TARGET_FILE:${TARGET}>]==]\n"
      "      TESTS_FILE [==[${tests_file}]==]\n"
      "      INCLUDE_FILE \"\${CMAKE_CURRENT_LIST_FILE}\"\n"
      "      FAILURE_NAME [==[${placeholder}_DISCOVERY_FAILED]==]\n"
      "      CMAKE_COMMAND [==[${CMAKE_COMMAND}]==]\n"
      "      WORKING_DIRECTORY [==[${arg_WORKING_DIRECTORY}]==]\n"
      "      NAME [==[${arg_NAME}]==]\n"
      "      NAME_STRIP [==[${arg_NAME_STRIP}]==]\n"
      "      RUNTIMES [==[${arg_RUNTIMES}]==]\n"
      "      INCLUDE [==[${arg_INCLUDE}]==]\n"
      "      EXCLUDE [==[${arg_EXCLUDE}]==]\n"
      "      ARGS [==[${arg_ARGS}]==]\n"
      "      PROPERTIES [==[${arg_PROPERTIES}]==]\n"
      "      COST_BY_NAME [==[${arg_COST_BY_NAME}]==])\n"
      "   if(EXISTS [==[${tests_file}]==])\n"
      "      include([==[${tests_file}]==])\n"
      "   endif()\n"
      "else()\n"
      "   add_test([==[${placeholder}_NOT_BUILT]==] [==[${placeholder}_NOT_BUILT]==])\n")
   if(arg_PROPERTIES)
      string(APPEND content
         "   set_tests_properties([==[${placeholder}_NOT_BUILT]==] PROPERTIES${placeholder_properties})\n")
   endif()
   string(APPEND content "endif()\n")

   # A multi-config generator gets one script per configuration, chosen by the configuration ctest runs (-C).
   if(multi_config)
      foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
         file(GENERATE OUTPUT "${file_base}_include-${config}.cmake" CONTENT "${content}"
              CONDITION $<CONFIG:${config}>)
      endforeach()
      file(WRITE "${file_base}_include.cmake" "include(\"${file_base}_include-\${CTEST_CONFIGURATION_TYPE}.cmake\")\n")
   else()
      file(GENERATE OUTPUT "${file_base}_include.cmake" CONTENT "${content}")
   endif()
   set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${file_base}_include.cmake")
endfunction()
