if(BUILD_SYSTEM_CONTRACTS)
    add_contract(shadowtoken shadowtoken shadowtoken.cpp)
    target_include_directories(shadowtoken
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../sysio.opp.common/include>)
endif()
