set(BOOST_VERSION 1.89.0)
# Ask for the version installed via vcpkg (or omit the version to accept any)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

set(Boost_USE_STATIC_LIBS ON)
set(BOOST_COMPONENTS
        system
        container
        process
        filesystem
        iostreams
        date_time
        thread
        chrono
        context
        coroutine
        program_options
        interprocess
        unit_test_framework
        dll
        beast
        bimap
        multi_index
        signals2
        multiprecision
        hana
        property_tree
        lockfree
        assign
        accumulators
        rational
        format
        asio
        headers
        regex
        atomic
)
foreach (COMPONENT ${BOOST_COMPONENTS})
#    list(APPEND BOOST_COMPONENTS_REQUIRED ${COMPONENT})
    find_package(boost_${COMPONENT} ${BOOST_VERSION} EXACT CONFIG REQUIRED)
endforeach()
#find_package(boost_container ${BOOST_VERSION} CONFIG REQUIRED)
#find_package(Boost ${BOOST_VERSION} CONFIG REQUIRED
#        COMPONENTS
#        system
#        container
#        process
#
#    filesystem
#    iostreams
#    date_time
#    thread
#    chrono
#    context
#    coroutine
#    program_options
#    interprocess
#
#    unit_test_framework
#    dll
#    beast
#    bimap
#    multi_index
#    signals2
#    multiprecision
#    hana
#    property_tree
#    lockfree
#    assign
#    accumulators
#    rational
#    format
#    asio
#    headers
#    regex
#    atomic
#)

# Keep uBLAS shim if code links Boost::numeric_ublas
if (NOT TARGET boost_numeric_ublas)
  add_library(boost_numeric_ublas INTERFACE)
  add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)
endif()
