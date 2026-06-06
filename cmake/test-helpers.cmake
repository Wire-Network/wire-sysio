set(SYSIO_TEST_PORT_OFFSET_START 100)
set(SYSIO_TEST_PORT_OFFSET_STRIDE 192)
set(SYSIO_TEST_PORT_SHARD_BASE 8888)

# Host-specific reservations are opt-in so CI and fresh build directories keep deterministic compact shard allocation.
set(SYSIO_TEST_FORBIDDEN_PORTS "" CACHE STRING "Semicolon-separated actual TCP ports that test port shards must not overlap")
option(SYSIO_DETECT_LISTENING_TEST_PORTS "Avoid test port shards that overlap TCP ports listening during CMake configure" OFF)

# Append TCP listener ports that are present during configure; this is a local developer convenience, not a CI contract.
function(append_listening_test_ports out_var)
   set(listening_ports)

   if(APPLE)
      execute_process(
         COMMAND lsof -nP -iTCP -sTCP:LISTEN
         OUTPUT_VARIABLE listening_ports_output
         ERROR_QUIET
      )
   elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      execute_process(
         COMMAND ss -H -ltn
         OUTPUT_VARIABLE listening_ports_output
         ERROR_QUIET
      )
   endif()

   if(listening_ports_output)
      string(REGEX MATCHALL "[:.]([0-9]+)([ \t\r\n]|$)" listening_port_matches "${listening_ports_output}")
      foreach(listening_port_match IN LISTS listening_port_matches)
         string(REGEX REPLACE "^[:.]([0-9]+).*" "\\1" listening_port "${listening_port_match}")
         if(listening_port MATCHES "^[0-9]+$")
            list(APPEND listening_ports "${listening_port}")
         endif()
      endforeach()
      list(REMOVE_DUPLICATES listening_ports)
   endif()

   set(${out_var} ${${out_var}} ${listening_ports} PARENT_SCOPE)
endfunction()

if(SYSIO_DETECT_LISTENING_TEST_PORTS)
   append_listening_test_ports(SYSIO_TEST_FORBIDDEN_PORTS)
endif()

function(test_port_offset_overlaps_forbidden_port out_var port_offset)
   math(EXPR shard_first_port "${SYSIO_TEST_PORT_SHARD_BASE} + ${port_offset}")
   math(EXPR shard_last_port "${shard_first_port} + ${SYSIO_TEST_PORT_OFFSET_STRIDE} - 1")

   set(overlaps_forbidden_port FALSE)
   foreach(forbidden_port IN LISTS SYSIO_TEST_FORBIDDEN_PORTS)
      if(forbidden_port GREATER_EQUAL shard_first_port AND forbidden_port LESS_EQUAL shard_last_port)
         set(overlaps_forbidden_port TRUE)
         break()
      endif()
   endforeach()

   set(${out_var} ${overlaps_forbidden_port} PARENT_SCOPE)
endfunction()

function(next_test_port_offset out_var)
   get_property(next_offset GLOBAL PROPERTY SYSIO_NEXT_TEST_PORT_OFFSET)
   if(NOT next_offset)
      set(next_offset "${SYSIO_TEST_PORT_OFFSET_START}")
   endif()

   test_port_offset_overlaps_forbidden_port(overlaps_forbidden_port "${next_offset}")
   while(overlaps_forbidden_port)
      math(EXPR next_offset "${next_offset} + ${SYSIO_TEST_PORT_OFFSET_STRIDE}")
      test_port_offset_overlaps_forbidden_port(overlaps_forbidden_port "${next_offset}")
   endwhile()

   math(EXPR next_next_offset "${next_offset} + ${SYSIO_TEST_PORT_OFFSET_STRIDE}")
   set_property(GLOBAL PROPERTY SYSIO_NEXT_TEST_PORT_OFFSET ${next_next_offset})
   set(${out_var} ${next_offset} PARENT_SCOPE)
endfunction()

function(setup_test_common)
   cmake_parse_arguments(PARSE_ARGV 0 arg "AUTO_PORT_OFFSET;AUTO_LR_PORT_OFFSET" "NAME;COST;TIMEOUT;PORT_OFFSET" "COMMAND;ENVIRONMENT")

   add_test(NAME "${arg_NAME}" COMMAND ${arg_COMMAND} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

   if(arg_COST)
      set_tests_properties("${arg_NAME}" PROPERTIES COST ${arg_COST})
   endif()
   if(arg_TIMEOUT)
      set_tests_properties("${arg_NAME}" PROPERTIES TIMEOUT ${arg_TIMEOUT})
   endif()
   if(arg_PORT_OFFSET)
      set(test_port_offset ${arg_PORT_OFFSET})
   elseif(arg_AUTO_LR_PORT_OFFSET)
      next_test_port_offset(test_port_offset)
   elseif(arg_AUTO_PORT_OFFSET)
      next_test_port_offset(test_port_offset)
   endif()
   set(test_environment ${arg_ENVIRONMENT})
   if(DEFINED test_port_offset)
      list(APPEND test_environment "SYSIO_TEST_PORT_OFFSET=${test_port_offset}")
   endif()
   if(test_environment)
      set_tests_properties("${arg_NAME}" PROPERTIES ENVIRONMENT "${test_environment}")
   endif()
endfunction()

function(add_p_test)
   setup_test_common(${ARGV})
endfunction()

function(add_wasmspec_test)
   cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;COST;TIMEOUT" "COMMAND")

   setup_test_common(${ARGV})
   set_property(TEST "${arg_NAME}" PROPERTY LABELS wasm_spec_tests)
endfunction()

function(add_np_test)
   cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;COST;TIMEOUT" "COMMAND")

   setup_test_common(${ARGV} AUTO_PORT_OFFSET)
   set_property(TEST "${arg_NAME}" PROPERTY LABELS nonparallelizable_tests)
endfunction()

function(add_lr_test)
   cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;COST;TIMEOUT" "COMMAND")

   setup_test_common(${ARGV} AUTO_LR_PORT_OFFSET)
   set_property(TEST "${arg_NAME}" PROPERTY LABELS long_running_tests)
endfunction()
