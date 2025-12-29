#pragma once

#include <boost/test/unit_test.hpp>

struct scoped_boost_log_level
{
   boost::unit_test::log_level prev;

   explicit scoped_boost_log_level(boost::unit_test::log_level lvl)
       : prev(boost::unit_test::unit_test_log.set_threshold_level(lvl))
   {

   }

   ~scoped_boost_log_level()
   {
      boost::unit_test::unit_test_log.set_threshold_level(prev);
   }
};
