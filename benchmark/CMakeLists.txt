file(GLOB BENCHMARK "*.cpp")
add_executable( benchmark ${BENCHMARK} )

target_link_libraries( benchmark sysio_testing fc Boost::program_options bn256::bn256)
target_include_directories( benchmark PUBLIC
                            "${CMAKE_CURRENT_SOURCE_DIR}"
                            "${CMAKE_CURRENT_BINARY_DIR}/../unittests/include"
                          )

# Standalone dedup head-to-head benchmark. Links only sysio_chain + chainbase (no testing
# harness / contracts), so it builds without the system-contract WASMs. Not built by default.
add_executable( dedup_bench EXCLUDE_FROM_ALL dedup/dedup_bench.cpp )
target_link_libraries( dedup_bench sysio_chain chainbase fc )
