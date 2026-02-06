#include <fc/log/logger.hpp>
#include <fc/log/log_message.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/log/dmlog_sink.hpp>
#include <spdlog/sinks/sink.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>
#include <vector>

namespace fc {

   inline static logger the_default_logger;

   constexpr const char* DEFAULT_PATTERN = "%^%-5l %Y-%m-%dT%T.%f %-9!k %20!s:%-5# %-20!! ] %v%$";

   class thread_name_formatter_flag : public spdlog::custom_flag_formatter {
      public:
         void format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& dest) override {
            const std::string& some_txt = fc::get_thread_name();
            // Use spdlog::details::scoped_padder to apply alignment
            // The padinfo_ member holds the width and alignment settings from the pattern
            spdlog::details::scoped_padder p(some_txt.size(), padinfo_, dest);
            spdlog::details::fmt_helper::append_string_view(some_txt, dest);
         }

         std::unique_ptr<custom_flag_formatter> clone() const override {
            return spdlog::details::make_unique<thread_name_formatter_flag>();
         }
   };

   logger::impl::impl() :_parent(nullptr) {
      auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_st>();
      sink->set_color(spdlog::level::debug, sink->green);
      sink->set_color(spdlog::level::info, sink->reset);
      sink->set_color(spdlog::level::warn, sink->yellow);
      sink->set_color(spdlog::level::err, sink->red);
      _agent_logger = std::make_unique<spdlog::logger>( "", sink );

      auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
      formatter->add_flag<thread_name_formatter_flag>('k').set_pattern(DEFAULT_PATTERN);
      _agent_logger->set_formatter(std::move(formatter));

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
       auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
       formatter->add_flag<thread_name_formatter_flag>('k').set_pattern(DEFAULT_PATTERN);
       my->_agent_logger->set_formatter(std::move(formatter));
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
