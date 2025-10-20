cmake_minimum_required(VERSION 3.8)
project(contracts)

if(BUILD_SYSTEM_CONTRACTS)

  find_package(cdt REQUIRED)
  add_subdirectory(sysio.bios)
  add_subdirectory(sysio.system)
  add_subdirectory(sysio.msig)
  add_subdirectory(sysio.roa)

  add_subdirectory(sysio.token)
  add_subdirectory(sysio.wrap)
  add_subdirectory(test_contracts)
  if(NOT CDT_BUILD AND BUILD_TEST_CONTRACTS)
    add_subdirectory(tests)
  endif()

endif()