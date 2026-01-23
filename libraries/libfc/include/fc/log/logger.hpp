#pragma once
#include <fc/time.hpp>
#include <fc/log/log_message.hpp>

// define `SPDLOG_ACTIVE_LEVEL` before including spdlog.h as per https://github.com/gabime/spdlog/issues/1268
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#define SPDLOG_LEVEL_NAMES { "trace", "debug", "info", "warn", "error", "crit", "off" }

#include <spdlog/spdlog.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/fmt/fmt.h>

#include <cstddef>
#include <string>
#include <vector>
#include <memory>

namespace fc
{
   constexpr std::string DEFAULT_LOGGER = "default";

   /**
    @code
      void my_class::func() 
      {
         fc_dlog( my_class_logger, "Format four: ${arg}  five: ${five}", ("arg",4)("five",5) );
      }
    @endcode
    */
   class logger
   {
      public:
         static logger& default_logger();
         static logger get( const std::string& name = DEFAULT_LOGGER );
         static void update( const std::string& name, logger& log );

         logger();
         logger( const std::string& name, const logger& parent = nullptr );
         logger( std::nullptr_t );
         logger( const logger& c );
         logger( logger&& c ) noexcept;
         ~logger();
         logger& operator=(const logger&);
         logger& operator=(logger&&) noexcept;
         friend bool operator==( const logger&, nullptr_t );
         friend bool operator!=( const logger&, nullptr_t );

         logger&    set_log_level( log_level e );
         log_level  get_log_level()const;
         logger&    set_parent( const logger& l );
         logger     get_parent()const;

         std::unique_ptr<spdlog::logger>& get_agent_logger()const;
         void update_agent_logger(std::unique_ptr<spdlog::logger>&& al);

         void  set_name( const std::string& n );
         std::string get_name()const;

         void set_enabled( bool e );
         bool is_enabled( log_level e )const;
         bool is_enabled()const;
         void log( log_message m );

      private:
         friend struct log_config;
         void add_sink(const std::shared_ptr<spdlog::sinks::sink>& s);
         std::vector<std::shared_ptr<spdlog::sinks::sink>>& get_sinks() const;

      private:
         class impl;
         std::shared_ptr<impl> my;
   };

} // namespace fc

// suppress warning "conditional expression is constant" in the while(0) for visual c++
// http://cnicholson.net/2009/03/stupid-c-tricks-dowhile0-and-c4127/
#define FC_MULTILINE_MACRO_BEGIN do {
#ifdef _MSC_VER
# define FC_MULTILINE_MACRO_END \
    __pragma(warning(push)) \
    __pragma(warning(disable:4127)) \
    } while (0) \
    __pragma(warning(pop))
#else
# define FC_MULTILINE_MACRO_END  } while (0)
#endif

#define fc_tlog( LOGGER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::all ) ) \
      SPDLOG_LOGGER_TRACE((LOGGER).get_agent_logger(), FC_FMT( FORMAT, ##__VA_ARGS__ )); \
  FC_MULTILINE_MACRO_END

#define fc_dlog( LOGGER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::debug ) ) \
      SPDLOG_LOGGER_DEBUG((LOGGER).get_agent_logger(), FC_FMT( FORMAT, ##__VA_ARGS__ )); \
  FC_MULTILINE_MACRO_END

#define fc_ilog( LOGGER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::info ) ) \
      SPDLOG_LOGGER_INFO((LOGGER).get_agent_logger(), FC_FMT( FORMAT, ##__VA_ARGS__ )); \
  FC_MULTILINE_MACRO_END

#define fc_wlog( LOGGER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::warn ) ) \
      SPDLOG_LOGGER_WARN((LOGGER).get_agent_logger(), FC_FMT( FORMAT, ##__VA_ARGS__ )); \
  FC_MULTILINE_MACRO_END

#define fc_elog( LOGGER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::error ) ) \
      SPDLOG_LOGGER_ERROR((LOGGER).get_agent_logger(), FC_FMT( FORMAT, ##__VA_ARGS__ )); \
  FC_MULTILINE_MACRO_END

#define tlog( FORMAT, ... ) \
   fc_tlog( fc::logger::default_logger(), FORMAT, ##__VA_ARGS__)

#define dlog( FORMAT, ... ) \
   fc_dlog( fc::logger::default_logger(), FORMAT, ##__VA_ARGS__)

#define ilog( FORMAT, ... ) \
   fc_ilog( fc::logger::default_logger(), FORMAT, ##__VA_ARGS__)

#define wlog( FORMAT, ... ) \
   fc_wlog( fc::logger::default_logger(), FORMAT, ##__VA_ARGS__)

#define elog( FORMAT, ... ) \
   fc_elog( fc::logger::default_logger(), FORMAT, ##__VA_ARGS__)

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/punctuation/comma_if.hpp>

#define FC_FORMAT_ARG(r, unused, base) \
  BOOST_PP_STRINGIZE(base) ": {} "

#define FC_FORMAT_ARG_PARAM(r, unused, i, base) \
  BOOST_PP_COMMA_IF(i) base

#define FC_FORMAT( SEQ )\
    BOOST_PP_SEQ_FOR_EACH( FC_FORMAT_ARG, v, SEQ ) 

#define FC_FORMAT_ARG_PARAMS( SEQ )\
    BOOST_PP_SEQ_FOR_EACH_I( FC_FORMAT_ARG_PARAM, _, SEQ )

#define idump( SEQ ) \
    ilog( FC_FORMAT(SEQ), FC_FORMAT_ARG_PARAMS(SEQ) )  
#define wdump( SEQ ) \
    wlog( FC_FORMAT(SEQ), FC_FORMAT_ARG_PARAMS(SEQ) )  
#define edump( SEQ ) \
    elog( FC_FORMAT(SEQ), FC_FORMAT_ARG_PARAMS(SEQ) )  

// this disables all normal logging statements -- not something you'd normally want to do,
// but it's useful if you're benchmarking something and suspect logging is causing
// a slowdown.
#ifdef FC_DISABLE_LOGGING
# undef ulog
# define ulog(...) FC_MULTILINE_MACRO_BEGIN FC_MULTILINE_MACRO_END
# undef elog
# define elog(...) FC_MULTILINE_MACRO_BEGIN FC_MULTILINE_MACRO_END
# undef wlog
# define wlog(...) FC_MULTILINE_MACRO_BEGIN FC_MULTILINE_MACRO_END
# undef ilog
# define ilog(...) FC_MULTILINE_MACRO_BEGIN FC_MULTILINE_MACRO_END
# undef dlog
# define dlog(...) FC_MULTILINE_MACRO_BEGIN FC_MULTILINE_MACRO_END
#endif