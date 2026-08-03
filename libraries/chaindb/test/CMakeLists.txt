file(GLOB UNIT_TESTS CONFIGURE_DEPENDS "*.cpp")
add_executable( chainbase_test ${UNIT_TESTS}  )
target_link_libraries( chainbase_test  chainbase Boost::unit_test_framework ${PLATFORM_SPECIFIC_LIBS} )

add_test(chainbase_test chainbase_test)
