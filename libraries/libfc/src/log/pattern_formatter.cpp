#include <fc/log/pattern_formatter.hpp>
#include <fc/log/logger_config.hpp>

#include <spdlog/pattern_formatter.h>

#include <string>

namespace fc {

namespace {

class thread_name_formatter_flag : public spdlog::custom_flag_formatter {
public:
   void format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& dest) override {
      const std::string& name = fc::get_thread_name();
      spdlog::details::scoped_padder p(name.size(), padinfo_, dest);
      spdlog::details::fmt_helper::append_string_view(name, dest);
   }

   std::unique_ptr<custom_flag_formatter> clone() const override {
      return spdlog::details::make_unique<thread_name_formatter_flag>();
   }
};

} // anonymous namespace

std::unique_ptr<spdlog::formatter> make_pattern_formatter(std::string_view pattern) {
   auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
   formatter->add_flag<thread_name_formatter_flag>('k').set_pattern(std::string{pattern});
   return formatter;
}

} // namespace fc
