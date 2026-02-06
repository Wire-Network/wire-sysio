bootstrap_contract(sysio.roa)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.roa sysio.roa sysio.roa.cpp)

   target_include_directories(sysio.roa
      PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include)
endif()
