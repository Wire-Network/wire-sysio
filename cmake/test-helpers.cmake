# Network tests run under tests/bind_registry.py, which leases a block of ports from the host's bind registry, runs the
# test as the leader of a new session with the lease in SYSIO_TEST_PORTS, and releases the lease once the session has
# exited.
find_program(SYSIO_TEST_PYTHON python3 REQUIRED)
set(SYSIO_TEST_PORT_LEASE_WRAPPER "${CMAKE_BINARY_DIR}/tests/bind_registry.py")

# Register a test. PORT_LEASE runs it under the port-lease wrapper; NO_PORT_LEASE overrides PORT_LEASE for a network
# label's test that must not be wrapped (the wrapper's own self-tests).
function(setup_test_common)
   cmake_parse_arguments(PARSE_ARGV 0 arg "PORT_LEASE;NO_PORT_LEASE" "NAME;COST;TIMEOUT" "COMMAND;ENVIRONMENT")

   if(arg_PORT_LEASE AND NOT arg_NO_PORT_LEASE)
      set(wrapped_command ${arg_COMMAND})
      list(GET wrapped_command 0 test_executable)
      # add_test only resolves a target name in the first word of COMMAND, which is now the wrapper, so resolve it
      # here. if(TARGET) only sees targets defined before this call: define a test's executable before registering it,
      # or the bare name is passed through and the test fails to start.
      if(TARGET "${test_executable}")
         list(REMOVE_AT wrapped_command 0)
         list(PREPEND wrapped_command "$<TARGET_FILE:${test_executable}>")
      endif()
      add_test(NAME "${arg_NAME}"
               COMMAND "${SYSIO_TEST_PYTHON}" "${SYSIO_TEST_PORT_LEASE_WRAPPER}" ${wrapped_command}
               WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
   else()
      add_test(NAME "${arg_NAME}" COMMAND ${arg_COMMAND} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
   endif()

   if(arg_COST)
      set_tests_properties("${arg_NAME}" PROPERTIES COST ${arg_COST})
   endif()
   if(arg_TIMEOUT)
      set_tests_properties("${arg_NAME}" PROPERTIES TIMEOUT ${arg_TIMEOUT})
   endif()
   if(arg_ENVIRONMENT)
      set_tests_properties("${arg_NAME}" PROPERTIES ENVIRONMENT "${arg_ENVIRONMENT}")
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

# Nonparallelizable network test, run under a port lease unless NO_PORT_LEASE is given.
function(add_np_test)
   cmake_parse_arguments(PARSE_ARGV 0 arg "NO_PORT_LEASE" "NAME;COST;TIMEOUT" "COMMAND;ENVIRONMENT")

   setup_test_common(${ARGV} PORT_LEASE)
   set_property(TEST "${arg_NAME}" PROPERTY LABELS nonparallelizable_tests)
endfunction()

# Long-running network test, run under a port lease unless NO_PORT_LEASE is given.
function(add_lr_test)
   cmake_parse_arguments(PARSE_ARGV 0 arg "NO_PORT_LEASE" "NAME;COST;TIMEOUT" "COMMAND;ENVIRONMENT")

   setup_test_common(${ARGV} PORT_LEASE)
   set_property(TEST "${arg_NAME}" PROPERTY LABELS long_running_tests)
endfunction()
