configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/sysio/chain/core_symbol.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/sysio/chain/core_symbol.hpp)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/genesis_state_root_key.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/genesis_state_root_key.cpp)

file(GLOB HEADERS "include/sysio/chain/*.hpp"
                  "include/sysio/chain/webassembly/*.hpp"
                  "${CMAKE_CURRENT_BINARY_DIR}/include/sysio/chain/core_symbol.hpp" )

if((APPLE AND UNIX) OR (${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD"))
   set(PLATFORM_TIMER_IMPL platform_timer_kqueue.cpp)
else()
   try_run(POSIX_TIMER_TEST_RUN_RESULT POSIX_TIMER_TEST_COMPILE_RESULT ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/platform_timer_posix_test.c)
   if(POSIX_TIMER_TEST_RUN_RESULT EQUAL 0)
      set(PLATFORM_TIMER_IMPL platform_timer_posix.cpp)
      set(CHAIN_RT_LINKAGE rt)
   else()
      set(PLATFORM_TIMER_IMPL platform_timer_asio_fallback.cpp)
   endif()
endif()

if("sys-vm-oc" IN_LIST SYSIO_WASM_RUNTIMES)
   enable_language(ASM)
   set(CHAIN_SYSVMOC_SOURCES webassembly/runtimes/sys-vm-oc/code_cache.cpp
                             webassembly/runtimes/sys-vm-oc/executor.cpp
                             webassembly/runtimes/sys-vm-oc/memory.cpp
                             webassembly/runtimes/sys-vm-oc/intrinsic.cpp
                             webassembly/runtimes/sys-vm-oc/LLVMJIT.cpp
                             webassembly/runtimes/sys-vm-oc/LLVMEmitIR.cpp
                             webassembly/runtimes/sys-vm-oc/compile_monitor.cpp
                             webassembly/runtimes/sys-vm-oc/compile_trampoline.cpp
                             webassembly/runtimes/sys-vm-oc/ipc_helpers.cpp
                             webassembly/runtimes/sys-vm-oc/gs_seg_helpers.c
                             webassembly/runtimes/sys-vm-oc/stack.cpp
                             webassembly/runtimes/sys-vm-oc/switch_stack_linux.s
                             webassembly/runtimes/sys-vm-oc.cpp
                             webassembly/runtimes/sys-vm-oc/default_real_main.cpp)

   set_source_files_properties(webassembly/runtimes/sys-vm-oc/LLVMJIT.cpp PROPERTIES COMPILE_FLAGS "--std=gnu++17")
   set_source_files_properties(webassembly/runtimes/sys-vm-oc/LLVMEmitIR.cpp PROPERTIES COMPILE_FLAGS "--std=gnu++17")
   set_source_files_properties(webassembly/runtimes/sys-vm-oc/switch_stack_linux.s PROPERTIES COMPILE_FLAGS "-Wno-unused-command-line-argument")

   if(LLVM_VERSION VERSION_LESS 7.1 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      enable_language(ASM-LLVMWAR)
      list(APPEND CHAIN_SYSVMOC_SOURCES webassembly/runtimes/sys-vm-oc/llvmWARshim.llvmwar)
      set_source_files_properties(webassembly/runtimes/sys-vm-oc/llvmWARshim.llvmwar PROPERTIES COMPILE_FLAGS "--std=gnu++17")
   else()
      list(APPEND CHAIN_SYSVMOC_SOURCES webassembly/runtimes/sys-vm-oc/llvmWARshim.cpp)
      set_source_files_properties(webassembly/runtimes/sys-vm-oc/llvmWARshim.cpp PROPERTIES COMPILE_FLAGS "--std=gnu++17")
   endif()

   llvm_map_components_to_libnames(LLVM_LIBS support core passes mcjit native orcjit)
   include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
   add_definitions(${LLVM_DEFINITIONS})

   option(SYSVMOC_ENABLE_DEVELOPER_OPTIONS "enable developer options for SYS VM OC" OFF)
endif()

if("sys-vm" IN_LIST SYSIO_WASM_RUNTIMES OR "sys-vm-jit" IN_LIST SYSIO_WASM_RUNTIMES)
   set(CHAIN_SYSVM_SOURCES "webassembly/runtimes/sys-vm.cpp")
   set(CHAIN_SYSVM_LIBRARIES sys-vm)
endif()

set(CHAIN_WEBASSEMBLY_SOURCES
   webassembly/action.cpp
   webassembly/authorization.cpp
   webassembly/cf_system.cpp
   webassembly/cf_transaction.cpp
   webassembly/compiler_builtins.cpp
   webassembly/context_free.cpp
   webassembly/console.cpp
   webassembly/crypto.cpp
   webassembly/database.cpp
   webassembly/memory.cpp
   webassembly/permission.cpp
   webassembly/privileged.cpp
   webassembly/producer.cpp
   webassembly/softfloat.cpp
   webassembly/system.cpp
   webassembly/transaction.cpp
)

add_library(sysio_rapidjson INTERFACE)
target_include_directories(sysio_rapidjson INTERFACE ../rapidjson/include)

## SORT .cpp by most likely to change / break compile
add_library( sysio_chain
             merkle.cpp
             name.cpp
             transaction.cpp
             block.cpp
             block_header.cpp
             block_header_state.cpp
             block_state.cpp
             fork_database.cpp
             controller.cpp
             authorization_manager.cpp
             resource_limits.cpp
             block_log.cpp
             block_root_processor.cpp
             transaction_context.cpp
             sysio_contract.cpp
             sysio_contract_abi.cpp
             sysio_contract_abi_bin.cpp
             chain_config.cpp
             chain_id_type.cpp
             genesis_state.cpp
             ${CMAKE_CURRENT_BINARY_DIR}/genesis_state_root_key.cpp

              wast_to_wasm.cpp
              wasm_interface.cpp
              wasm_sysio_validation.cpp
              wasm_sysio_injection.cpp
              wasm_config.cpp
              apply_context.cpp
              abi_serializer.cpp
              asset.cpp
              snapshot.cpp
              snapshot_scheduler.cpp
              deep_mind.cpp

             ${CHAIN_SYSVMOC_SOURCES}
             ${CHAIN_SYSVM_SOURCES}
             ${CHAIN_WEBASSEMBLY_SOURCES}

             authority.cpp
             trace.cpp
             transaction_metadata.cpp
             protocol_state_object.cpp
             protocol_feature_activation.cpp
             protocol_feature_manager.cpp
             producer_schedule.cpp
             genesis_intrinsics.cpp
             symbol.cpp
             whitelisted_intrinsics.cpp
             thread_utils.cpp
             platform_timer_accuracy.cpp
             ${PLATFORM_TIMER_IMPL}
             ${HEADERS}
             )

## Boost::accumulators depends on Boost::numeric_ublas, which is still missing cmake support (see
## https://github.com/boostorg/cmake/issues/39). Until this is fixed, manually add Boost::numeric_ublas
## as an interface library
## ----------------------------------------------------------------------------------------------------
add_library(boost_numeric_ublas INTERFACE)
add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)

target_link_libraries( sysio_chain PUBLIC bn256 fc chainbase sysio_rapidjson Logging IR WAST WASM
                       softfloat builtins ${CHAIN_SYSVM_LIBRARIES} ${LLVM_LIBS} ${CHAIN_RT_LINKAGE}
                       Boost::signals2 Boost::hana Boost::property_tree Boost::multi_index Boost::asio Boost::lockfree
                       Boost::assign Boost::accumulators
                     )
target_include_directories( sysio_chain
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-jit/Include"
                            )

add_library(sysio_chain_wrap INTERFACE )
target_link_libraries(sysio_chain_wrap INTERFACE sysio_chain)

if("sys-vm-oc" IN_LIST SYSIO_WASM_RUNTIMES)
   target_link_libraries(sysio_chain_wrap INTERFACE "-Wl,-wrap=main")
endif()

foreach(RUNTIME ${SYSIO_WASM_RUNTIMES})
   string(TOUPPER "${RUNTIME}" RUNTIMEUC)
   string(REPLACE "-" "_" RUNTIMEUC ${RUNTIMEUC})
   target_compile_definitions(sysio_chain PUBLIC "SYSIO_${RUNTIMEUC}_RUNTIME_ENABLED")
endforeach()

if(SYSVMOC_ENABLE_DEVELOPER_OPTIONS)
   message(WARNING "SYS VM OC Developer Options are enabled; these are NOT supported")
   target_compile_definitions(sysio_chain PUBLIC SYSIO_SYS_VM_OC_DEVELOPER)
endif()

install( TARGETS sysio_chain
   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT dev EXCLUDE_FROM_ALL
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
)
install( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/sysio/chain/
      DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/sysio/chain
      COMPONENT dev EXCLUDE_FROM_ALL
      FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" EXCLUDE
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/sysio/chain/core_symbol.hpp DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/sysio/chain COMPONENT dev EXCLUDE_FROM_ALL)
