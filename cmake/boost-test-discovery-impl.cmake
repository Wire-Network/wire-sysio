# Implementation of boost_test_discover_tests() (boost-test-discovery.cmake). ctest runs it from the include script
# that function generates, to list the executable's top-level units and write the CTest script registering them.

# ctest reads test files with no policies set; functions keep the policies in effect where they are defined.
cmake_policy(PUSH)
cmake_policy(VERSION 3.19)

set(_BOOST_TEST_LIST_TIMEOUT_SECONDS 60)
# Longer than one discovery can take, so a process waiting for the lock outlasts the one holding it.
set(_BOOST_TEST_LOCK_TIMEOUT_SECONDS 180)
# Stands for "no runtime" in the runtime loop: an empty element cannot be appended to an empty CMake list.
set(_BOOST_TEST_NO_RUNTIME "<none>")

# Quote a value as a CMake bracket argument; the trailing `]` covers a value ending in the start of the closing bracket.
function(_boost_test_quote out value)
   set(equals "")
   string(FIND "${value}]" "]${equals}]" found)
   while(NOT found EQUAL -1)
      string(APPEND equals "=")
      string(FIND "${value}]" "]${equals}]" found)
   endwhile()
   set(${out} "[${equals}[${value}]${equals}]" PARENT_SCOPE)
endfunction()

# Append `<command>(<quoted arguments>)` to the script held in the caller's `script` variable.
function(_boost_test_append_command command)
   set(line "${command}(")
   foreach(argument IN LISTS ARGN)
      _boost_test_quote(quoted "${argument}")
      string(APPEND line " ${quoted}")
   endforeach()
   string(APPEND script "${line})\n")
   set(script "${script}" PARENT_SCOPE)
endfunction()

# Append an entry that fails, echoing `message`, to the caller's `script`; ARGN holds its extra properties.
function(_boost_test_append_failure name cmake_command message)
   _boost_test_append_command(add_test "${name}" "${cmake_command}" -E echo "${message}")
   _boost_test_append_command(set_tests_properties "${name}" PROPERTIES WILL_FAIL TRUE ${ARGN})
   set(script "${script}" PARENT_SCOPE)
endfunction()

function(boost_test_discover_tests_impl)
   # Lists arrive as single `;`-joined arguments; the ${ARGN} form splits them where PARSE_ARGV would not.
   set(single_values EXECUTABLE TESTS_FILE INCLUDE_FILE FAILURE_NAME CMAKE_COMMAND WORKING_DIRECTORY NAME INCLUDE
       EXCLUDE LOCK_TIMEOUT)
   cmake_parse_arguments(arg "" "${single_values}" "NAME_STRIP;RUNTIMES;ARGS;PROPERTIES;COST_BY_NAME" ${ARGN})

   # One ctest process discovers at a time, and one that waited finds the script already current. Without the lock the
   # shared files are not this process's to touch, so it keeps the current list and reports why.
   if(NOT DEFINED arg_LOCK_TIMEOUT)
      set(arg_LOCK_TIMEOUT ${_BOOST_TEST_LOCK_TIMEOUT_SECONDS})
   endif()
   file(LOCK "${arg_TESTS_FILE}.lock" GUARD FUNCTION TIMEOUT ${arg_LOCK_TIMEOUT} RESULT_VARIABLE lock_result)
   if(NOT lock_result STREQUAL "0")
      set(script "")
      _boost_test_append_failure("${arg_FAILURE_NAME}" "${arg_CMAKE_COMMAND}"
                                 "cannot lock ${arg_TESTS_FILE}.lock: ${lock_result}" ${arg_PROPERTIES})
      cmake_language(EVAL CODE "${script}")
      return()
   endif()
   # The script is stale when the executable or the include script is newer (a tie counts) or this implementation
   # changed.
   if(EXISTS "${arg_TESTS_FILE}" AND
      NOT "${arg_EXECUTABLE}" IS_NEWER_THAN "${arg_TESTS_FILE}" AND
      NOT "${arg_INCLUDE_FILE}" IS_NEWER_THAN "${arg_TESTS_FILE}" AND
      "${arg_TESTS_FILE}" IS_NEWER_THAN "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
      return()
   endif()

   # The report sink keeps the listing apart from anything the executable prints, and overrides
   # BOOST_TEST_REPORT_SINK in the environment.
   set(listing "${arg_TESTS_FILE}.listing")
   file(REMOVE "${listing}")
   execute_process(COMMAND "${arg_EXECUTABLE}" --list_content "--report_sink=${listing}"
                   WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}" TIMEOUT ${_BOOST_TEST_LIST_TIMEOUT_SECONDS}
                   RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
   set(text "")
   if(result EQUAL 0 AND EXISTS "${listing}")
      file(READ "${listing}" text)
   endif()
   file(REMOVE "${listing}")

   # Boost prints one unit per line: `<name>*` when enabled, `<name> ` when disabled by default, optionally followed
   # by `: <description>`, with nested units indented. Nested lines and descriptions are cut before the text becomes a
   # CMake list, whose `;` and `[` handling would otherwise split or merge lines. The sink holds nothing but the
   # listing, so a line without a marker is still a unit, registered enabled.
   string(REGEX REPLACE "\n [^\n]*" "" text "\n${text}")
   string(REGEX REPLACE "([* ]): [^\n]*" "\\1" text "${text}")
   string(REGEX MATCHALL "[^\n]+" lines "${text}")

   set(script "# Generated from `${arg_EXECUTABLE} --list_content` by boost-test-discovery-impl.cmake.\n")
   set(listed 0)
   set(test_names "")
   set(duplicates "")
   foreach(line IN LISTS lines)
      if(line MATCHES "^(.+)\\*$")
         set(UNIT "${CMAKE_MATCH_1}")
         set(enabled TRUE)
      elseif(line MATCHES "^(.+) $")
         set(UNIT "${CMAKE_MATCH_1}")
         set(enabled FALSE)
      else()
         set(UNIT "${line}")
         set(enabled TRUE)
      endif()
      math(EXPR listed "${listed} + 1")
      if((arg_INCLUDE AND NOT UNIT MATCHES "${arg_INCLUDE}") OR (arg_EXCLUDE AND UNIT MATCHES "${arg_EXCLUDE}"))
         continue()
      endif()

      set(NAME "${UNIT}")
      foreach(pattern IN LISTS arg_NAME_STRIP)
         string(REGEX REPLACE "${pattern}" "" NAME "${NAME}")
      endforeach()
      set(cost "")
      list(LENGTH arg_COST_BY_NAME cost_values)
      set(index 0)
      while(index LESS cost_values AND NOT cost)
         list(GET arg_COST_BY_NAME ${index} pattern)
         math(EXPR index "${index} + 1")
         if(NAME MATCHES "${pattern}")
            list(GET arg_COST_BY_NAME ${index} cost)
         endif()
         math(EXPR index "${index} + 1")
      endwhile()

      set(runtimes ${arg_RUNTIMES})
      if(NOT runtimes)
         set(runtimes "${_BOOST_TEST_NO_RUNTIME}")
      endif()
      foreach(RUNTIME IN LISTS runtimes)
         set(command "${arg_EXECUTABLE}" "--run_test=${UNIT}" ${arg_ARGS})
         if(RUNTIME STREQUAL _BOOST_TEST_NO_RUNTIME)
            set(RUNTIME "")
         else()
            list(APPEND command "--" "--${RUNTIME}")
         endif()
         string(CONFIGURE "${arg_NAME}" test_name @ONLY)
         if(test_name IN_LIST test_names)
            list(APPEND duplicates "${test_name}")
         endif()
         list(APPEND test_names "${test_name}")
         _boost_test_append_command(add_test "${test_name}" ${command})
         set(properties WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}" ${arg_PROPERTIES})
         if(cost)
            list(APPEND properties COST ${cost})
         endif()
         if(NOT enabled)
            list(APPEND properties DISABLED TRUE)
         endif()
         _boost_test_append_command(set_tests_properties "${test_name}" PROPERTIES ${properties})
      endforeach()
   endforeach()

   if(listed EQUAL 0)
      # Register the failure directly and leave no tests file, so the next ctest run discovers again.
      file(REMOVE "${arg_TESTS_FILE}")
      set(script "")
      _boost_test_append_failure("${arg_FAILURE_NAME}" "${arg_CMAKE_COMMAND}"
                                 "${arg_EXECUTABLE} --list_content listed no test units (exit ${result}): ${output}"
                                 ${arg_PROPERTIES})
      cmake_language(EVAL CODE "${script}")
      return()
   endif()
   if(duplicates)
      # Units whose names coincide after NAME_STRIP would share one CTest name; report it until the executable changes.
      list(REMOVE_DUPLICATES duplicates)
      _boost_test_append_failure("${arg_FAILURE_NAME}" "${arg_CMAKE_COMMAND}"
                                 "${arg_EXECUTABLE}: units map to the same test name: ${duplicates}" ${arg_PROPERTIES})
   endif()
   # Written beside the target and renamed into place, so a reader never includes a partial file.
   file(WRITE "${arg_TESTS_FILE}.tmp" "${script}")
   file(RENAME "${arg_TESTS_FILE}.tmp" "${arg_TESTS_FILE}")
endfunction()

cmake_policy(POP)
