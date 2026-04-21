#include <fc/log/logger.hpp>
#include <fc/log/log_message.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/log/dmlog_sink.hpp>
#include <fc/log/pattern_formatter.hpp>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>
#include <vector>

namespace fc {

   static logger the_default_logger;

   logger::impl::impl() :_parent(nullptr) {
      auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_st>();
      sink->set_color(spdlog::level::debug, sink->green);
      sink->set_color(spdlog::level::info, sink->reset);
      sink->set_color(spdlog::level::warn, sink->yellow);
      sink->set_color(spdlog::level::err, sink->red);
      sink->set_formatter(fc::log::make_pattern_formatter());
      _agent_logger = std::make_unique<spdlog::logger>( "", sink );
      _agent_logger->set_level(spdlog::level::info);
   }

    logger::logger()
    :my( new impl() ){}

    logger::logger( const std::string& name, const logger& parent )
    :my( new impl() ) {
       my->_name = name;
       my->_parent = parent.my;
    }

    bool operator==( const logger& l, std::nullptr_t ) { return !l.my; }
    bool operator!=( const logger& l, std::nullptr_t ) { return !!l.my;  }

    void logger::set_name( const std::string& n ) { my->_name = n; }
    std::string logger::get_name()const { return my->_name; }

    logger& logger::default_logger() {
       return the_default_logger;
    }

    void logger::update( const std::string& name, logger& log ) {
       log_config::update_logger( name, log );
    }

    logger  logger::get_parent()const { return logger{my->_parent}; }
    logger& logger::set_parent(const logger& p) { my->_parent = p.my; return *this; }

    logger& logger::set_log_level(log_level ll) {
       my->_level = ll;
       switch (ll) {
       case fc::log_level::values::all:
          my->_agent_logger->set_level(spdlog::level::trace);
          break;
       case fc::log_level::values::debug:
          my->_agent_logger->set_level(spdlog::level::debug);
          break;
       case fc::log_level::values::info:
          my->_agent_logger->set_level(spdlog::level::info);
          break;
       case fc::log_level::values::warn:
          my->_agent_logger->set_level(spdlog::level::warn);
          break;
       case fc::log_level::values::error:
          my->_agent_logger->set_level(spdlog::level::err);
          break;
       case fc::log_level::values::off:
          my->_agent_logger->set_level(spdlog::level::off);
          break;
       }
       return *this;
    }

    std::unique_ptr<spdlog::logger>& logger::get_agent_logger() const { return my->_agent_logger; };

    void logger::update_agent_logger(std::unique_ptr<spdlog::logger>&& al) {
       my->_agent_logger = std::move(al);
       set_log_level(my->_level);
    };

    void logger::add_sink(const std::shared_ptr<spdlog::sinks::sink>& s) {
       my->_sinks.push_back(s);
    };

    std::vector<std::shared_ptr<spdlog::sinks::sink> >& logger::get_sinks() const {
       return my->_sinks;
    }

   bool do_default_config = configure_logging( logging_config::default_config() );

} // namespace fc
