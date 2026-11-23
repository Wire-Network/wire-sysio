set(BOOST_VERSION 1.92.0)
# Ask for the version installed via vcpkg (or omit the version to accept any)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

set(Boost_USE_STATIC_LIBS ON)
set(BOOST_COMPONENTS
        accumulators
        algorithm
        asio
        assign
        beast
        bimap
        chrono
        container
        date_time
        format
        hana
        headers
        interprocess
        iostreams
        lexical_cast
        lockfree
        multi_index
        multiprecision
        process
        program_options
        property_tree
        rational
        signals2
        system
        thread
        unit_test_framework
)
foreach (COMPONENT ${BOOST_COMPONENTS})
    find_package(boost_${COMPONENT} ${BOOST_VERSION} EXACT CONFIG REQUIRED)
endforeach()
