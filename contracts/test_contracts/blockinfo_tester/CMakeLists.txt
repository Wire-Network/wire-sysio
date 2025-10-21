if(NOT BUILD_SYSTEM_CONTRACTS OR NOT CDT_BUILD)
  # To rebuild blockinfo_tester.wasm
  # cdt-cpp  -I/home/kevin/ext/wire-sysio/contracts/test_contracts/blockinfo_tester/../../sysio.system/include
  #          -I/home/kevin/ext/wire-sysio/contracts/test_contracts/blockinfo_tester/..
  #          -O3 --no-abigen -MD -MT test_contracts/blockinfo_tester/CMakeFiles/blockinfo_tester.dir/blockinfo_tester.cpp.obj
  #          -MF test_contracts/blockinfo_tester/CMakeFiles/blockinfo_tester.dir/blockinfo_tester.cpp.obj.d
  #          -o test_contracts/blockinfo_tester/CMakeFiles/blockinfo_tester.dir/blockinfo_tester.cpp.obj
  #          -c /home/kevin/ext/wire-sysio/contracts/test_contracts/blockinfo_tester/blockinfo_tester.cpp
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/blockinfo_tester.wasm ${CMAKE_CURRENT_BINARY_DIR}/blockinfo_tester.wasm COPYONLY)
endif()
