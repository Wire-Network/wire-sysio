bootstrap_contract(sysio.swap)
if(BUILD_SYSTEM_CONTRACTS)
   add_contract(sysio.swap sysio.swap sysio.swap.cpp token_functions.cpp)
   target_include_directories(sysio.swap
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
endif()
