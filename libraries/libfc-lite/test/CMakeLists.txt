
file(GLOB_RECURSE SRC_FILES *.cpp *.hpp)

add_executable(test_fc_lite
        ${SRC_FILES}
)
target_link_libraries(test_fc_lite
        fc-lite
        Boost::multiprecision
        Boost::thread
        Boost::unit_test_framework
)

add_test(
        NAME test_fc_lite
        COMMAND test_fc_lite
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
