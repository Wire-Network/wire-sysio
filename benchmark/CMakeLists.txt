file(GLOB BENCHMARK "*.cpp")
add_executable( benchmark ${BENCHMARK} )

# trace_api_plugin brings the process_block / process_block_to_json implementations
# used by the trace_api_json benchmark; its include path pulls in the data_log_entry
# variant type needed by the synthetic trace builder.
target_link_libraries( benchmark sysio_testing fc Boost::program_options bn256::bn256 trace_api_plugin)
target_include_directories( benchmark PUBLIC
                            "${CMAKE_CURRENT_SOURCE_DIR}"
                            "${CMAKE_CURRENT_BINARY_DIR}/../unittests/include"
                          )

# ABI fixture paths consumed by abi_serializer.cpp.  These point at the system-contract
# sources, not the built artifacts, so the benchmark works regardless of build state.
target_compile_definitions( benchmark
   PRIVATE
      TOKEN_ABI_PATH="${CMAKE_SOURCE_DIR}/contracts/sysio.token/sysio.token.abi"
      SYSTEM_ABI_PATH="${CMAKE_SOURCE_DIR}/contracts/sysio.system/sysio.system.abi"
)

# Standalone dedup head-to-head benchmark. Links only sysio_chain + chainbase (no testing
# harness / contracts), so it builds without the system-contract WASMs. Not built by default.
add_executable( dedup_bench EXCLUDE_FROM_ALL dedup/dedup_bench.cpp )
target_link_libraries( dedup_bench sysio_chain chainbase fc )
