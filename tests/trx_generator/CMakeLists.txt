add_executable(trx_generator main.cpp trx_generator.cpp trx_provider.cpp)

target_include_directories(trx_generator PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(trx_generator PRIVATE sysio_chain fc Boost::program_options ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS})

add_executable(trx_generator_tests trx_generator_tests.cpp trx_provider.cpp trx_generator.cpp)
target_link_libraries(trx_generator_tests PRIVATE sysio_chain fc Boost::program_options ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS})
target_include_directories(trx_generator_tests PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(trx_generator_tests trx_generator_tests)
set_tests_properties(trx_generator_tests PROPERTIES LABELS nonparallelizable_failing_tests RUN_SERIAL TRUE)
