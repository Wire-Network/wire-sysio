file(GLOB UNIT_TESTS CONFIGURE_DEPENDS "*.cpp")

add_executable( http_plugin_unit_tests ${UNIT_TESTS})

target_link_libraries( http_plugin_unit_tests
                       PRIVATE appbase
                       PRIVATE http_plugin
                       ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS} )

# Header-only chain dependencies for the stored-exception status-code test:
# bind_stream.hpp needs sysio/chain/plugin_interface.hpp (chain_interface has no
# CMake target -- see producer_plugin's CMakeLists for the same pattern) plus the
# chain exception / t_or_exception headers. Everything used is inline, so include
# dirs suffice; linking sysio_chain would drag fc's PUBLIC Boost::unit_test_framework
# onto the link line and collide with this test's header-only Boost.Test.
target_include_directories( http_plugin_unit_tests PRIVATE
                            ${CMAKE_SOURCE_DIR}/plugins/http_plugin/include
                            ${CMAKE_SOURCE_DIR}/plugins/chain_interface/include
                            ${CMAKE_SOURCE_DIR}/libraries/chain/include
                            ${CMAKE_BINARY_DIR}/libraries/chain/include
                            ${CMAKE_SOURCE_DIR}/libraries/chaindb/include
                            ${CMAKE_SOURCE_DIR}/tests )

add_np_test( NAME http_plugin_unit_tests COMMAND http_plugin_unit_tests )
