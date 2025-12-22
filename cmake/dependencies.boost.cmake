set(BOOST_VERSION 1.89.0)
# Ask for the version installed via vcpkg (or omit the version to accept any)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

set(Boost_USE_STATIC_LIBS ON)
set(BOOST_COMPONENTS
        accumulators
        asio
        assign
        atomic
        beast
        bimap
        chrono
        container
        context
        coroutine
        date_time
        dll
        filesystem
        format
        hana
        headers
        interprocess
        iostreams
        lockfree
        multi_index
        multiprecision
        process
        program_options
        property_tree
        rational
        regex
        signals2
        system
        thread
        unit_test_framework
)
foreach (COMPONENT ${BOOST_COMPONENTS})
    find_package(boost_${COMPONENT} ${BOOST_VERSION} EXACT CONFIG REQUIRED)
endforeach()

# Keep uBLAS shim if code links Boost::numeric_ublas
if (NOT TARGET boost_numeric_ublas)
  add_library(boost_numeric_ublas INTERFACE)
  add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)
endif()
