# Like reenter_deposit, the regression consumes committed artifacts under the source tree.
if(BUILD_SYSTEM_CONTRACTS)
    add_contract(reenter_desyndicate reenter_desyndicate reenter_desyndicate.cpp)
endif()
