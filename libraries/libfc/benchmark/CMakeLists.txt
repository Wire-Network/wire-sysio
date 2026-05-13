add_executable(variant_bench EXCLUDE_FROM_ALL variant_bench.cpp)

target_link_libraries(variant_bench
   fc
   ${PLATFORM_SPECIFIC_LIBS}
)
