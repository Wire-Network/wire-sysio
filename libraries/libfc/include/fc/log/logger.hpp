#pragma once

#include <fc/log/log_message.hpp>
#include <fc/time.hpp>

#include <cstddef>
#include <string>
#include <vector>
#include <memory>

namespace spdlog::sinks {
class sink;
}

namespace fc
{
   /**
    @code
      void my_class::func() {
         fc_dlog( my_class_logger, "Format four: {} five: {}", 4, 5 );
      }
    @endcode
    */
   class logger
   {
      public:
         static logger& default_logger();
         static void update( const std::string& name, logger& log );

         logger();
         explicit logger( const std::string& name, const logger& parent = logger{nullptr} );
         explicit logger( std::nullptr_t ) {}
         logger( const logger& c ) = default;
         logger( logger&& c ) noexcept = default;
         ~logger() = default;
         logger& operator=(const logger&) = default;
         logger& operator=(logger&&) noexcept = default;
         friend bool operator==( const logger&, nullptr_t );
         friend bool operator!=( const logger&, nullptr_t );

         logger&    set_log_level( log_level e );
         log_level  get_log_level()const { return my->_level; }
         logger&    set_parent( const logger& l );
         logger     get_parent()const;

         std::unique_ptr<spdlog::logger>& get_agent_logger()const;
         void update_agent_logger(std::unique_ptr<spdlog::logger>&& al);

         void  set_name( const std::string& n );
         std::string get_name()const;

         void set_enabled( bool e ) { my->_enabled = e; }
         bool is_enabled( log_level e )const { return my->_enabled && e >= my->_level; }
         bool is_enabled()const { return my->_enabled; }

      private:
         friend struct log_config;
         void add_sink(const std::shared_ptr<spdlog::sinks::sink>& s);
         std::vector<std::shared_ptr<spdlog::sinks::sink>>& get_sinks() const;

         class impl {
         public:
            impl();

            std::string                     _name;
            bool                            _enabled = true;
            log_level                       _level = log_level::info;
            std::shared_ptr<impl>           _parent;
            std::unique_ptr<spdlog::logger> _agent_logger;
            std::vector<std::shared_ptr<spdlog::sinks::sink>> _sinks;
         };

         explicit logger( std::shared_ptr<impl> impl ) : my( std::move( impl ) ) {}

      private:
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