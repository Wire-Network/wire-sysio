find_package(Boost ${BOOST_VERSION}
  EXACT
  CONFIG
  REQUIRED
  COMPONENTS
  assign
  system
  multiprecision
  signals2
  beast
  property_tree
  headers
  hana
  bimap
  multi_index
  filesystem
  dll
  lockfree
  process
  program_options
  iostreams
  date_time
  regex
  thread
  chrono
  context
  coroutine
  atomic
  interprocess
  unit_test_framework
)

add_library(boost_numeric_ublas INTERFACE)
add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)

find_package(Boost ${BOOST_VERSION}
  EXACT
  CONFIG
  REQUIRED
  COMPONENTS
  accumulators
)
