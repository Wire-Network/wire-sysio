include(CMakeParseArguments)

macro(unittest_target TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCE_FILES" ${ARGN})
  message(NOTICE "Creating unittest target (${TARGET}) with sources: ${ARG_SOURCE_FILES}")
  add_executable(${TARGET} ${ARG_SOURCE_FILES})

  target_link_libraries(
    ${TARGET}
    PUBLIC
          -Wl,${whole_archive_flag}

          ${PLUGIN_DEFAULT_DEPENDENCIES}
          -Wl,${no_whole_archive_flag}
#    ${PLUGIN_DEFAULT_DEPENDENCIES}

    sysio_chain_wrap
    state_history
    chainbase
    sysio_testing
    custom_appbase
    libsodium::libsodium
  )

  target_include_directories(${TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/libraries/testing/include
    ${CMAKE_BINARY_DIR}/contracts
    ${CMAKE_SOURCE_DIR}/contracts
    ${CMAKE_SOURCE_DIR}/unittests
    ${CMAKE_SOURCE_DIR}/unittests/system-test-contracts
    ${CMAKE_BINARY_DIR}/unittests/system-test-contracts
    ${CMAKE_BINARY_DIR}/unittests/include
    ${CMAKE_SOURCE_DIR}/plugins/http_plugin/include
    ${CMAKE_SOURCE_DIR}/plugins/chain_interface/include)

  # Export intrinsic symbols for native-module contract .so loading.
  # Generate the --dynamic-list file (once) and add linker flag to export
  # INTRINSIC_EXPORT symbols so dlopen'd contract .so files can resolve them.
  if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
    set(NATIVE_INTRINSIC_EXPORTS_SRC "${CMAKE_SOURCE_DIR}/libraries/testing/native_intrinsic_exports.cpp")
    set(NATIVE_EXPORT_LIST "${CMAKE_BINARY_DIR}/native_intrinsic_exports.list")

    if(NOT TARGET native_export_list)
      add_custom_command(
        OUTPUT "${NATIVE_EXPORT_LIST}"
        COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_export_list.py"
          --format=dynamic-list -o "${NATIVE_EXPORT_LIST}"
          "${NATIVE_INTRINSIC_EXPORTS_SRC}"
        DEPENDS "${NATIVE_INTRINSIC_EXPORTS_SRC}" "${CMAKE_SOURCE_DIR}/scripts/gen_export_list.py"
        COMMENT "Generating native intrinsic export list"
        VERBATIM
      )
      add_custom_target(native_export_list DEPENDS "${NATIVE_EXPORT_LIST}")
    endif()

    add_dependencies(${TARGET} native_export_list)

    # Link intrinsic exports with --whole-archive so the extern "C" symbols
    # are included even though nothing in the binary references them directly.
    # They're resolved at dlopen time by native contract .so files.
    target_link_libraries(${TARGET} PUBLIC
      -Wl,${whole_archive_flag}
      native_intrinsic_exports
      -Wl,${no_whole_archive_flag}
    )

    if(NOT APPLE)
      target_link_options(${TARGET} PRIVATE
        "-Wl,--dynamic-list=${NATIVE_EXPORT_LIST}")
      # Override the global -static-libgcc (from linker-config.cmake) for test
      # executables that load native contract .so files. Both the executable and
      # the .so must use the SAME shared libgcc_s.so for C++ exception unwinding
      # to work across the dlopen boundary. Without this, each gets its own
      # static copy of _Unwind_* / __gxx_personality_v0, causing SIGABRT when
      # exceptions propagate through .so stack frames.
      target_link_libraries(${TARGET} PUBLIC gcc_s stdc++)
    else()
      target_link_options(${TARGET} PRIVATE
        "-Wl,-exported_symbols_list,${NATIVE_EXPORT_LIST}")
    endif()
  endif()

endmacro()

macro(unittest_tests_add TARGET)
  cmake_parse_arguments(ARG "" "" "UNITTEST_FILES" ${ARGN})
  foreach(TEST_SUITE ${ARG_UNITTEST_FILES}) # create an independent target for each test suite
    # GET TEST SUITE NAME
    execute_process(
      COMMAND
      bash -c
      "grep -E 'BOOST_AUTO_TEST_SUITE\\s*[(]' ${TEST_SUITE} | grep -vE '//.*BOOST_AUTO_TEST_SUITE\\s*[(]' | cut -d ')' -f 1 | cut -d '(' -f 2"
      OUTPUT_VARIABLE SUITE_NAME
      OUTPUT_STRIP_TRAILING_WHITESPACE) # get the test suite name from the *.cpp file

    # IF NOT EMPTY, ADD TESTS
    if(NOT "" STREQUAL "${SUITE_NAME}") # ignore empty lines
      # TRIM TEST SUITE NAME
      execute_process(
        COMMAND bash -c "echo ${SUITE_NAME} | sed -e 's/s$//' | sed -e 's/_test$//'"
        OUTPUT_VARIABLE TRIMMED_SUITE_NAME
        OUTPUT_STRIP_TRAILING_WHITESPACE) # trim "_test" or "_tests" from the end of ${SUITE_NAME}

      # to run ${TARGET} with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose"
      foreach(RUNTIME ${SYSIO_WASM_RUNTIMES})
        # Skip native-module from the general test loop — only specific test suites
        # with native .so contracts available should be registered for native-module.
        if(NOT RUNTIME STREQUAL "native-module")
          add_test(NAME ${TRIMMED_SUITE_NAME}_${TARGET}_${RUNTIME} COMMAND ${TARGET} --run_test=${SUITE_NAME} --report_level=detailed --color_output -- --${RUNTIME})
        endif()
      endforeach()

    endif()
  endforeach()
endmacro()